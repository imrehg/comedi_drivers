/*
 *  Copyright (C) 2009 Gergely Imreh, imrehg@gmail.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/usb.h>
#include "../comedidev.h"

/* Total number of usbdux devices */
#define NUMNIUSB            16

#define BOARDNAME "ni_usb"

#define DRIVER_VERSION "v0.1"
#define DRIVER_AUTHOR "Gergely Imreh <imrehg@gmail.com>"
#define DRIVER_DESC "NI USB-6259 -- imrehg@gmail.com"


struct ni_usb_sub {
	/* attached? */
	int attached;
	/* is it associated with a subdevice? */
	int probed;
	/* pointer to the usb-device */
	struct usb_device *usbdev;
	/* actual number of in-buffers */
	int numOfInBuffers;
	/* actual number of out-buffers */
	int numOfOutBuffers;
	/* ISO-transfer handling: buffers */
	struct urb **urbIn;
	struct urb **urbOut;
	/* pwm-transfer handling */
	struct urb *urbPwm;
	/* PWM period */
	unsigned int pwmPeriod;
	/* PWM internal delay for the GPIF in the FX2 */
	int8_t pwmDelay;
	/* size of the PWM buffer which holds the bit pattern */
	int sizePwmBuf;
	/* input buffer for the ISO-transfer */
	int16_t *inBuffer;
	/* input buffer for single insn */
	int16_t *insnBuffer;
	/* output buffer for single DA outputs */
	int16_t *outBuffer;
	/* interface number */
	int ifnum;
	/* interface structure in 2.6 */
	struct usb_interface *interface;
	/* comedi device for the interrupt context */
	struct comedi_device *comedidev;
	/* is it USB_SPEED_HIGH or not? */
	short int high_speed;
	/* asynchronous command is running */
	short int ai_cmd_running;
	short int ao_cmd_running;
	/* pwm is running */
	short int pwm_cmd_running;
	/* continous aquisition */
	short int ai_continous;
	short int ao_continous;
	/* number of samples to aquire */
	int ai_sample_count;
	int ao_sample_count;
	/* time between samples in units of the timer */
	unsigned int ai_timer;
	unsigned int ao_timer;
	/* counter between aquisitions */
	unsigned int ai_counter;
	unsigned int ao_counter;
	/* interval in frames/uframes */
	unsigned int ai_interval;
	/* D/A commands */
	int8_t *dac_commands;
	/* commands */
	int8_t *dux_commands;
	struct semaphore sem;
};

static struct ni_usb_sub ni_usb_sub[NUMNIUSB];

static DECLARE_MUTEX(start_stop_sem);

/* allocate memory for the urbs and initialise them */
static int ni_usb_sub_probe(struct usb_interface *uinterf,
			   const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(uinterf);
	struct device *dev = &uinterf->dev;
	int i;
	int index;

	dev_dbg(dev, "comedi_: ni_usb_: "
		"finding a free structure for the usb-device\n");

	down(&start_stop_sem);
	/* look for a free place in the usbdux array */
	index = -1;
	for (i = 0; i < NUMNIUSB; i++) {
		if (!(ni_usb_sub[i].probed)) {
			index = i;
			break;
		}
	}

	/* no more space */
	if (index == -1) {
		dev_err(dev, "Too many usbdux-devices connected.\n");
		up(&start_stop_sem);
		return -EMFILE;
	}
	dev_dbg(dev, "comedi_: ni_usb: "
		"ni_usb[%d] is ready to connect to comedi.\n", index);

	init_MUTEX(&(ni_usb_sub[index].sem));
	/* save a pointer to the usb device */
	ni_usb_sub[index].usbdev = udev;

	/* 2.6: save the interface itself */
	ni_usb_sub[index].interface = uinterf;
	/* get the interface number from the interface */
	ni_usb_sub[index].ifnum = uinterf->altsetting->desc.bInterfaceNumber;
	/* hand the private data over to the usb subsystem */
	/* will be needed for disconnect */
	usb_set_intfdata(uinterf, &(ni_usb_sub[index]));

	dev_dbg(dev, "comedi_: ni_usb: ifnum=%d\n", ni_usb_sub[index].ifnum);

	/* test if it is high speed (USB 2.0) */
	ni_usb_sub[index].high_speed =
		(ni_usb_sub[index].usbdev->speed == USB_SPEED_HIGH);

	ni_usb_sub[index].probed = 1;
	up(&start_stop_sem);

	dev_info(dev, "comedi_: ni_usb%d "
		 "has been successfully initialised.\n", index);
	/* success */
	return 0;
}


static void ni_usb_sub_disconnect(struct usb_interface *intf)
{
	struct ni_usb_sub *ni_usb_sub_tmp = usb_get_intfdata(intf);
	struct usb_device *udev = interface_to_usbdev(intf);

	if (!ni_usb_sub_tmp) {
		dev_err(&intf->dev,
			"comedi_: disconnect called with null pointer.\n");
		return;
	}
	if (ni_usb_sub_tmp->usbdev != udev) {
		dev_err(&intf->dev,
			"comedi_: BUG! called with wrong ptr!!!\n");
		return;
	}
	comedi_usb_auto_unconfig(udev);
	down(&start_stop_sem);
	down(&ni_usb_sub_tmp->sem);
	up(&ni_usb_sub_tmp->sem);
	up(&start_stop_sem);
	dev_dbg(&intf->dev, "comedi_: disconnected from the usb\n");
}


/* is called when comedi-config is called */
static int ni_usb_attach(struct comedi_device *dev, struct comedi_devconfig *it)
{
	int index;
	int i;
	struct ni_usb_sub *udev;

	dev->private = NULL;

	down(&start_stop_sem);
	/* find a valid device which has been detected by the probe function of
	 * the usb */
	index = -1;
	for (i = 0; i < NUMNIUSB; i++) {
		if ((ni_usb_sub[i].probed) && (!ni_usb_sub[i].attached)) {
			index = i;
			break;
		}
	}

	if (index < 0) {
		printk(KERN_ERR "comedi%d: ni_usb: error: attach failed, no "
		       "ni_usb devs connected to the usb bus.\n", dev->minor);
		up(&start_stop_sem);
		return -ENODEV;
	}

    udev = &ni_usb_sub[index];
	down(&udev->sem);
	/* pointer back to the corresponding comedi device */
	udev->comedidev = dev;

    udev->attached = 1;

	up(&udev->sem);
    up(&start_stop_sem);

	dev_info(&udev->interface->dev, "comedi%d: attached to ni_usb.\n",
		 dev->minor);

	return 0;
}


static int ni_usb_detach(struct comedi_device *dev)
{
	struct ni_usb_sub *ni_usb_sub_tmp;

	if (!dev) {
		printk(KERN_ERR
			"comedi?: ni_usb: detach without dev variable...\n");
		return -EFAULT;
	}

	ni_usb_sub_tmp = dev->private;
	if (!ni_usb_sub_tmp) {
		printk(KERN_ERR
			"comedi?: ni_usb: detach without ptr to usbduxsub[]\n");
		return -EFAULT;
	}

	dev_dbg(&ni_usb_sub_tmp->interface->dev, "comedi%d: detach usb device\n",
		dev->minor);

	down(&ni_usb_sub_tmp->sem);
	/* Don't allow detach to free the private structure */
	/* It's one entry of of usbduxsub[] */
	dev->private = NULL;
	ni_usb_sub_tmp->attached = 0;
	ni_usb_sub_tmp->comedidev = NULL;
	up(&ni_usb_sub_tmp->sem);
	return 0;
}


/* main driver struct */
static struct comedi_driver driver_ni_usb = {
      .driver_name =	"ni_usb",
      .module =		THIS_MODULE,
      .attach =		ni_usb_attach,
      .detach =		ni_usb_detach,
};

/*
 * Table with the USB-devices
 */
static struct usb_device_id ni_usb_sub_table[] = {
	{ USB_DEVICE(0x3923, 0x7348) },
	{ }			/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, ni_usb_sub_table);


static struct usb_driver ni_usb_sub_driver = {
      .name =		BOARDNAME,
      .probe =		ni_usb_sub_probe,
      .disconnect =	ni_usb_sub_disconnect,
      .id_table =	ni_usb_sub_table,
};

static int __init init_ni_usb(void)
{
	printk(KERN_INFO KBUILD_MODNAME ": " DRIVER_VERSION ":" DRIVER_DESC "\n");
	usb_register(&ni_usb_sub_driver);
	comedi_driver_register(&driver_ni_usb);
	return 0;
}

/* deregistering the comedi driver and the usb-subsystem */
static void __exit exit_ni_usb(void)
{
	comedi_driver_unregister(&driver_ni_usb);
	usb_deregister(&ni_usb_sub_driver);
}

module_init(init_ni_usb);
module_exit(exit_ni_usb);

MODULE_AUTHOR("Greg");
MODULE_DESCRIPTION("NI USB-6259");
MODULE_LICENSE("GPL");
