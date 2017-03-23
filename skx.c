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
  int i, err;

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

  error = skx_init_output(interface, skx);
  if(error)
  {
    //Should free memory first!
    return -ENOMEM;
  }

  interrupt_in = &interface->cur_altsetting->endpoint[1].desc;

  usb_fill_int_urb(skx->interrupt_in, usb_dev,
    usb_rcvintpipe(usb_dev, interrupt_in->bEndpointAddress),
    skx->input_data, PKT_LEN, skx_interrupt_in,
    skx, interrupt_in);

  usb_set_intfdata(interface, skx);

  /*
    can the next two functions be combined?
  */
  error = skx_init_input(skx);
  if(error)
  {
    //Should free memory first!
    return -ENOMEM;
  }
  error = skx_start_input(skx);
  if(error)
  {
    //Should free memory first!
    return -ENOMEM;
  }

  /*
    Only missing part is the goto statements and their corresponding functions
    (xpad_deinit_input and xpad_deinit_output)
  */
}

/*
  Can be combined into the xpadone_process_packet function
  And possibly further combine the xpadone_process_buttons function into it
*/
static void skx_interrupt_in(struct urb *urb)
{
  struct usb_skx *skx = urb->context;
  struct device *d = &skx->interface->dev;
  int retval, status;

  err = urb->status;

  switch (err) {
  case 0:
    break;
  case -ECONNRESET:
  case -ENOENT:
  case -ESHUTDOWN:
    /* this urb is terminated, clean up */
    dev_dbg(d, "SKX:urb error occured err: %d\n",  status);
    return;
  default:
    dev_dbg(d, "%s - nonzero urb status received: %d\n",
      __func__, status);
    goto exit;
  }

  //Print this if we need to make sure something works
  //print_hex_dump(KERN_DEBUG, "xpad-dbg: ", DUMP_PREFIX_OFFSET, 32, 1, xpad->idata, XPAD_PKT_LEN, 0);
   
    struct input_dev *dev = skx->dev;

    switch(data[0]) {
      case 0x07:
        if(data[1]==0x30){
          unsigned long flags;
          struct output_packet *packet =
              &xpad->out_packets[XPAD_OUT_CMD_IDX];
          static const u8 mode_report_ack[] = {
            0x01, 0x20, 0x00, 0x09, 0x00, 0x07, 0x20, 0x02,
            0x00, 0x00, 0x00, 0x00, 0x00
          };

          spin_lock_irqsave(&xpad->odata_lock, flags);

          packet->len = sizeof(mode_report_ack);
          memcpy(packet->data, mode_report_ack, packet->len);
          packet->data[2] = data[2];
          packet->pending = true;

          /* Reset the sequence so we send out the ack now */
          xpad->last_out_packet = -1;
          xpad_try_sending_next_out_packet(xpad);

          spin_unlock_irqrestore(&xpad->odata_lock, flags);
        }
        input_report_key(dev, BTN_MODE, data[4] & 0x01);
        input_sync(dev);
        break;
      case 0x20:
        input_report_key(dev, BTN_START,  data[4] & 0x04);
        input_report_key(dev, BTN_SELECT, data[4] & 0x08);

        /* buttons A,B,X,Y */
        input_report_key(dev, BTN_A,  data[4] & 0x10);
        input_report_key(dev, BTN_B,  data[4] & 0x20);
        input_report_key(dev, BTN_X,  data[4] & 0x40);
        input_report_key(dev, BTN_Y,  data[4] & 0x80);

        /* DPAD Axis */
        input_report_abs(dev, ABS_HAT0X,
             !!(data[5] & 0x08) - !!(data[5] & 0x04));
        input_report_abs(dev, ABS_HAT0Y,
             !!(data[5] & 0x02) - !!(data[5] & 0x01));

        /* Stick Press Buttons */
        input_report_key(dev, BTN_THUMBL, data[5] & 0x40);
        input_report_key(dev, BTN_THUMBR, data[5] & 0x80);

        /* Triggers */
        input_report_key(dev, BTN_TL, data[5] & 0x10);
        input_report_key(dev, BTN_TR, data[5] & 0x20);

        /* Left Stick */
        input_report_abs(dev, ABS_X,
             (__s16) le16_to_cpup((__le16 *)(data + 10)));
        input_report_abs(dev, ABS_Y,
             ~(__s16) le16_to_cpup((__le16 *)(data + 12)));

        /* Right Stick */
        input_report_abs(dev, ABS_RX,
             (__s16) le16_to_cpup((__le16 *)(data + 14)));
        input_report_abs(dev, ABS_RY,
             ~(__s16) le16_to_cpup((__le16 *)(data + 16)));

        /*
          All Finished
        */
        input_sync(dev);
        break;
    }


exit:
  retval = usb_submit_urb(urb, GFP_ATOMIC);
  if (retval)
    dev_err(dev, "%s - usb_submit_urb failed with result %d\n",
      __func__, retval);
}

static void skx_disconnect(struct usb_interface *interface)
{
  return;
}
/*
  Need to traverse the xpad_init_output in order to gleam what functions are
  required for output (and what packets)
*/
static int skx_init_output(struct usb_interface *interface, struct usb_skx *skx)
{
  return 0;
}

static int skx_init_input(struct usb_skx *skx)
{
  /*
  cannot be combined with skx_start_input, as this is used to check for
  controller presence as well
  */
  return 0;
}

static int skx_start_input(struct usb_skx *skx)
{
  /*
    Should combine xpad_start_xbox_one and
    xpadone_send_init_pkt functions here
  */
  return 0;
}

static struct usb_driver skx_driver = {
	.name		= "skx",
	.probe		= skx_probe,
	.disconnect	= skx_disconnect,
	.id_table	= skx_table,
};

module_usb_driver(skx_driver);
