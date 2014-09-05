/*
 * Copyright (c) CERN 2014
 * Author: Federico Vaga <federico.vaga@cern.ch>
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fmc.h>
#include <linux/fmc-sdb.h>

#include "obsbox.h"

static struct fmc_driver ob_fmc_drv;
FMC_PARAM_BUSID(ob_fmc_drv);
FMC_PARAM_GATEWARE(ob_fmc_drv);

ZIO_PARAM_TRIGGER(ob_trigger);
ZIO_PARAM_BUFFER(ob_buffer);

/**
 * It gets from the SDB all the expected component addresses. If one of the
 * components is missing, then I assume that this is not the correct FPGA
 */
static int __ob_sdb_get_device(struct ob_dev *ob)
{
	struct fmc_device *fmc = ob->fmc;
	int ret;

	ret = fmc_scan_sdb_tree(fmc, 0);
	if (ret < 0) {
		dev_err(fmc->hwdev,
			"%s: no SDB in the bitstream."
			"Are you sure you've provided the correct one?\n",
			KBUILD_MODNAME);
		return ret;
	}

	/* Now use SDB to find the base addresses */
	ob->base_vic = fmc_find_sdb_device(fmc->sdb, 0xce42, 0x13, NULL);
	ob->base_dma_core = fmc_find_sdb_device(ob->fmc->sdb,
						0xce42, 0x601, NULL);
	ob->base_dma_irq = fmc_find_sdb_device(ob->fmc->sdb,
					       0xce42, 0xd5735ab4, NULL);
	ob->base_obs_core = fmc_find_sdb_device(ob->fmc->sdb,
						0xce42, 0xfb63f3f6, NULL);
	ob->base_obs_irq = fmc_find_sdb_device(ob->fmc->sdb,
					       0xce42, 0x21c5d7b1, NULL);
	if (!ob->base_vic || !ob->base_dma_core || !ob->base_dma_irq ||
	    !ob->base_obs_core || !ob->base_obs_irq) {
		dev_err(fmc->hwdev, "An expected SDB component is missing\n");
		return -EINVAL;
	}

	return ret;
}

static int ob_fmc_probe(struct fmc_device *fmc)
{
	struct ob_dev *ob;
	char *fwname;
	int err;

	/* Validate the new FMC device */
	err = fmc->op->validate(fmc, &ob_fmc_drv);
	if (err < 0) {
		dev_info(fmc->hwdev, "not using \"%s\" according to "
			 "modparam\n", KBUILD_MODNAME);
		return -ENODEV;
	}

	/* Driver data */
	ob = devm_kzalloc(&fmc->dev, sizeof(struct ob_dev), GFP_KERNEL);
	if (!ob)
		return -ENOMEM;
	fmc_set_drvdata(fmc, ob);
	ob->fmc = fmc;


	if (ob_fmc_drv.gw_n)
		fwname = "";	/* reprogram will pick from module parameter */
	else
		fwname = OB_DEFAULT_GATEWARE;
	dev_info(fmc->hwdev, "Gateware (%s)\n", fwname);

	/* We first write a new binary (and lm32) within the carrier */
	err = fmc->op->reprogram(fmc, &ob_fmc_drv, fwname);
	if (err) {
		dev_err(fmc->hwdev, "write firmware \"%s\": error %i\n",
				fwname, err);
	        return err;
	}
	dev_info(fmc->hwdev, "Gateware successfully loaded\n");

	err = __ob_sdb_get_device(ob);
	if (err < 0)
		return err;

	ob->hwzdev = zio_allocate_device();
	if (IS_ERR(ob->hwzdev)) {
		dev_err(fmc->hwdev, "Cannot allocate ZIO device\n");
		return PTR_ERR(ob->hwzdev);
	}
	/* Mandatory fields */
	ob->hwzdev->owner = THIS_MODULE;
	ob->hwzdev->priv_d = ob;
	/* Register the hardware zio_device */
	err = zio_register_device(ob->hwzdev, "obsbox",
				  ob->fmc->device_id);
	if (err) {
		dev_err(fmc->hwdev, "Cannot register ZIO device obs-box\n");
		zio_free_device(ob->hwzdev);
		return err;
	}

	return 0;
}

static int ob_fmc_remove(struct fmc_device *fmc)
{
	struct ob_dev *ob = fmc_get_drvdata(fmc);

	zio_unregister_device(ob->hwzdev);
	zio_free_device(ob->hwzdev);

	return 0;
}

static struct fmc_fru_id ob_fru_id[] = {
	{
		.product_name = "ObsBox",
	},
};

static struct fmc_driver ob_fmc_drv = {
	.version = FMC_VERSION,
	.driver.name = KBUILD_MODNAME,
	.probe = ob_fmc_probe,
	.remove = ob_fmc_remove,
	/*.id_table = {
		.fru_id = ob_fru_id,
		.fru_id_nr = ARRAY_SIZE(ob_fru_id),
		},*/
};



/* * * * * * * * * * * * * KERNEL  * * * * * * * * * * * * * * */

static int __init ob_init(void)
{
	int err;

	/*
	 * If the user choose a different buffer/trigger, then fix the template
	 * to apply the user choice
	 */
	if (ob_trigger)
		ob_tmpl.preferred_trigger = ob_trigger;
	if (ob_buffer)
		ob_tmpl.preferred_buffer = ob_buffer;

	/* Register the OBS-BOX ZIO driver */
        err = zio_register_driver(&ob_driver);
	if (err)
		return err;
	/* Register the OBS-BOX FMC driver */
	err = fmc_driver_register(&ob_fmc_drv);
	if (err) {
		zio_unregister_driver(&ob_driver);
		return err;
	}

	return 0;
}
static void __exit ob_exit(void)
{
	fmc_driver_unregister(&ob_fmc_drv);
	zio_unregister_driver(&ob_driver);
}

module_init(ob_init);
module_exit(ob_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Federico Vaga <federico.vaga@cern.com>");
MODULE_DESCRIPTION("OBS-BOX driver for ZIO framework");
MODULE_LICENSE("GPL");

CERN_SUPER_MODULE;
