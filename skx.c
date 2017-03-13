#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/usb/quirks.h>

MODULE_AUTHOR("Noah Steinberg and Jeremy Kielbiski");
MODULE_DESCRIPTION("A dedicated Xbox One S Controller driver");
MODULE_LICENSE("GPL");

#define PKT_LEN 64
#define DEV_NAME "Microsoft X-Box One S pad"
#define SKX_PROTOCOL() \
  .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
  .idVendor = 0x045e, \
  .bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
  .bInterfaceSubClass = 71, \
  .bInterfaceProtocol = 208

static struct usb_device_id skx_table[] = {
  {SKX_PROTOCOL()}
};

MODULE_DEVICE_TABLE(usb, skx_table);

struct output_packet {
  u8 data[PKT_LEN];
  u8 len;
  bool is_pending;
};

struct usb_skx {
  struct input_dev *dev;
  struct usb_device *usb_dev;
  struct usb_interface *interface;

  struct urb *interrupt_in;
  unsigned char *input_data;
  dma_addr_t input_data_dma;

  struct urb *interrupt_out;
  struct usb_anchor *interrupt_out_anchor;
  bool interrupt_out_active;
  u8 data_serial;
  unsigned char *odata;
  dma_addr_t odata_dma;
  spinlock_t odata_lock;

  struct output_packet out_packets[2];
  int last_out_packet;

  char phys_path[64];
};

static int skx_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
  struct usb_device *usb_dev = interface_to_usbdev(interface);
  struct usb_endpoint_descriptor *interrupt_in;
  struct usb_skx *skx;
  int interrupt_in_index, i, err;

  if(interface->cur_altsetting->desc.bNumEndpoints != 2)
  {
    return -ENODEV;
  }

  skx = kzalloc(sizeof(struct usb_skx), GFP_ATOMIC);
  if(!skx)
  {
    return -ENOMEM;
  }

  usb_make_path(usb_dev, skx->phys_path, sizeof(skx->phys_path));
  strlcat(skx->phys_path, "/input0", sizeof(skx->phys_path));

  skx->input_data = usb_alloc_coherent(usb_dev, PKT_LEN, GFP_ATOMIC,
    &skx->input_data_dma);
  if(!skx->input_data)
  {
    //Should free memory first!
    return -ENOMEM;
  }

  skx->interrupt_in = usb_alloc_urb(0, GFP_ATOMIC);
  if(!skx->interrupt_in)
  {
    //Should free memory first!
    return -ENOMEM;
  }

  skx->interface=interface;
  skx->usb_dev=usb_dev;

  if (interface->cur_altsetting->desc.bInterfaceNumber != 0) {
    //Should free memory first!
    return -ENODEV;
  }
  /*
    TODO: Add init functions for the proble (starting from line 1590 of xpad.c)
  */
}

static void skx_disconnect(struct usb_interface *interface)
{
  return;
}

static struct usb_driver skx_driver = {
	.name		= "skx",
	.probe		= skx_probe,
	.disconnect	= skx_disconnect,
	.id_table	= skx_table,
};

module_usb_driver(skx_driver);
