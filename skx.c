#define DEBUG
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/usb/quirks.h>
/*#include <linux/workqueue.h>*/

MODULE_AUTHOR("Noah Steinberg and Jeremy Kielbiski");
MODULE_DESCRIPTION("A dedicated Xbox One Controller driver");
MODULE_LICENSE("GPL");

/*#define DELAY_FF_SPRING 1*/

#define PKT_LEN 64
#define MAX_OUT_PACKETS 2
#define DEV_NAME "Microsoft X-Box One Controller"
#define SKX_PROTOCOL() \
  .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
  .idVendor = 0x045e, \
  .bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
  .bInterfaceSubClass = 71, \
  .bInterfaceProtocol = 208

static u8 lT_level = 0x00;
static int lT_overflow = 0;
static u8 rT_level = 0x00;
static int rT_overflow = 0;

static u8 lSX_level = 0x00;
static u8 rSX_level = 0x00;
static u8 lSY_level = 0x00;
static u8 rSY_level = 0x00;

/*static int delay_queue[64];

static struct workqueue_struct *skx_workqueue;
*/

static struct usb_device_id skx_table[] = {
  {SKX_PROTOCOL()},
  {}
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
  struct usb_anchor interrupt_out_anchor;
  bool interrupt_out_active;
  u8 data_serial;
  unsigned char *output_data;
  dma_addr_t output_data_dma;
  spinlock_t output_data_lock;

  struct output_packet out_packets[MAX_OUT_PACKETS];
  int last_out_packet;

  const char *name;
  char phys_path[64];
};

/*struct my_work
{
  struct work_struct wrk;
  struct usb_skx *skx;
};*/

static const signed short skx_buttons[] = {
  BTN_A, BTN_B, BTN_X, BTN_Y,
  BTN_START, BTN_SELECT,
  BTN_THUMBL, BTN_THUMBR,
  BTN_TL, BTN_TR,
  BTN_MODE,
  -1
};
static const signed short skx_axis[] = {
  ABS_X, ABS_Y,
  ABS_RX, ABS_RY,
  ABS_HAT0X, ABS_HAT0Y,
  ABS_Z, ABS_RZ,
  -1
};

static int skx_probe(struct usb_interface *interface, const struct usb_device_id *id);
static void skx_interrupt_in(struct urb *urb);
static void skx_interrupt_out(struct urb *urb);
static int skx_send_packet(struct usb_skx *skx);
static bool skx_prepare_packet(struct usb_skx *skx);
static void skx_disconnect(struct usb_interface *interface);
static int skx_init_output(struct usb_interface *interface, struct usb_skx *skx);
static int skx_init_input(struct usb_skx *skx);
static int skx_start_input(struct usb_skx *skx);
/*static void skx_delayed_action(struct work_struct*);*/

int skx_play_ff(struct input_dev *dev, void* data, struct ff_effect *effect)
{
  int err, ltx, lty, rtx, rty;
  __u16 s, w;
  struct usb_skx *skx = input_get_drvdata(dev);
  unsigned long flags;
  struct output_packet *packet = &skx->out_packets[1];
  /*int i;
  struct my_work *second,*third,*fourth,*fifth,*sixth,*seventh,*eigth,*ninth,*tenth;
  second = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  third = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  fourth = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  fifth = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  sixth = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  seventh = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  eigth = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  ninth = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  tenth = kzalloc(sizeof(struct my_work), GFP_KERNEL);
  second->skx = skx;
  third->skx = skx;
  fourth->skx = skx;
  fifth->skx = skx;
  sixth->skx = skx;
  seventh->skx = skx;
  eigth->skx = skx;
  ninth->skx = skx;
  tenth->skx = skx;*/
  spin_lock_irqsave(&skx->output_data_lock, flags);
  
  switch (effect->type){
    case FF_CONSTANT:
      s = effect->u.constant.level;
      w = effect->u.constant.level;
      dev_dbg(&dev->dev, "SKX: received FF_CONSTANT rumble request s: %d, w: %d, l: %d\n", s, w, effect->replay.length);
      packet->data[0] = 0x09;
      packet->data[1] = 0x00;
      packet->data[2] = skx->data_serial++;
      packet->data[3] = 0x09;
      packet->data[4] = 0x00;
      packet->data[5] = 0x0F;
      packet->data[6] = 0x00; // Left Trigger Strength MIN 00 MAX 0x64
      packet->data[7] = 0x00; // Right Trigger Strength MIN 00 MAX 0x64
      packet->data[8] = s; // Heavy Rumble Strength MIN 40 MAX 0x64, off 00
      packet->data[9] = w; // Light Rumble Strength MIN 40 MAX 0x64, off 00
      packet->data[10] = 0xFF; // Effect Length MIN 0x00 MAX FF
      packet->data[11] = 0x00; // Break Length MIN 0x00 MAX FF
      packet->data[12] = 0x00; // Number of additional effects  MIN 0x00 MAX FF
      packet->len = 13;
      break;
    case FF_RUMBLE:
      s = effect->u.rumble.strong_magnitude;
      w = effect->u.rumble.weak_magnitude;
      if(s> 0xFF)
        s=0xFF;
      if(w> 0xFF)
        w=0xFF;
      dev_dbg(&dev->dev, "SKX: received FF_RUMBLE request s: %d, w: %d, l: %d\n", s, w, effect->replay.length);
      packet->data[0] = 0x09;
      packet->data[1] = 0x00;
      packet->data[2] = skx->data_serial++;
      packet->data[3] = 0x09;
      packet->data[4] = 0x00;
      packet->data[5] = 0x0F;
      packet->data[6] = 0x00; // Left Trigger Strength MIN 00 MAX 0x64
      packet->data[7] = 0x00; // Right Trigger Strength MIN 00 MAX 0x64
      packet->data[8] = s; // Heavy Rumble Strength MIN 40 MAX 0x64, off 00
      packet->data[9] = w; // Light Rumble Strength MIN 40 MAX 0x64, off 00
      packet->data[10] = 0xFF; // Effect Length MIN 0x00 MAX FF
      packet->data[11] = 0x00; // Break Length MIN 0x00 MAX FF
      packet->data[12] = 0x00; // Number of additional effects  MIN 0x00 MAX FF
      packet->len = 13;
      break;
    case FF_SPRING:
      s = lT_overflow * 25 + lT_level / 0xA;
      w = rT_overflow * 25 + rT_level / 0xA;
      dev_dbg(&dev->dev, "SKX: received FF_SPRING request lT: %d rT: %d l: %d(L_Over: %d,L_Level: %d,R_Over: %d,R_Level: %d\n)", s, w, effect->replay.length, lT_overflow, lT_level, rT_overflow, rT_level);
      packet->data[0] = 0x09;
      packet->data[1] = 0x00;
      packet->data[2] = skx->data_serial++;
      packet->data[3] = 0x09;
      packet->data[4] = 0x00;
      packet->data[5] = 0x0F;
      packet->data[6] = s; // Left Trigger Strength MIN 00 MAX 0x64
      packet->data[7] = w; // Right Trigger Strength MIN 00 MAX 0x64
      packet->data[8] = 0x00; // Heavy Rumble Strength MIN 40 MAX 0x64, off 00
      packet->data[9] = 0x00; // Light Rumble Strength MIN 40 MAX 0x64, off 00
      packet->data[10] = 0x90; // Effect Length MIN 0x00 MAX FF
      packet->data[11] = 0x00; // Break Length MIN 0x00 MAX FF
      packet->data[12] = 0x00; // Number of additional effects  MIN 0x00 MAX FF
      packet->len = 13;
      /*INIT_WORK(&second->wrk, skx_delayed_action);
      INIT_WORK(&third->wrk, skx_delayed_action);
      INIT_WORK(&fourth->wrk, skx_delayed_action);
      INIT_WORK(&fifth->wrk, skx_delayed_action);
      INIT_WORK(&sixth->wrk, skx_delayed_action);
      INIT_WORK(&seventh->wrk, skx_delayed_action);
      INIT_WORK(&eigth->wrk, skx_delayed_action);
      INIT_WORK(&ninth->wrk, skx_delayed_action);
      INIT_WORK(&tenth->wrk, skx_delayed_action);
      for (i = 0; i < 9; i++)   
        delay_queue[i]=DELAY_FF_SPRING;
      queue_delayed_work(skx_workqueue, to_delayed_work(&second->wrk), 0x10);
      queue_delayed_work(skx_workqueue, to_delayed_work(&third->wrk), 0x20);
      queue_delayed_work(skx_workqueue, to_delayed_work(&fourth->wrk), 0x30);
      queue_delayed_work(skx_workqueue, to_delayed_work(&fifth->wrk), 0x40);
      queue_delayed_work(skx_workqueue, to_delayed_work(&sixth->wrk), 0x50);
      queue_delayed_work(skx_workqueue, to_delayed_work(&seventh->wrk), 0x60);
      queue_delayed_work(skx_workqueue, to_delayed_work(&eigth->wrk), 0x70);
      queue_delayed_work(skx_workqueue, to_delayed_work(&ninth->wrk), 0x80);
      queue_delayed_work(skx_workqueue, to_delayed_work(&tenth->wrk), 0x90);*/
      break;
    case FF_DAMPER:

      if(lSX_level > 128)
        ltx = -1 * (lSX_level - 255);
      else
        ltx = lSX_level;
      if(lSY_level > 128)
        lty = -1 * (lSY_level - 255);
      else
        lty = lSY_level;

      if(rSX_level > 128)
        rtx = -1 * (rSX_level - 255);
      else
        rtx = rSX_level;
      if(rSY_level > 128)
        rty = -1 * (rSY_level - 255);
      else
        rty = rSY_level;

      if((rtx+rty) > (ltx+lty))
        s = rtx+rty/3;
      else
        s = ltx+lty/3;

      dev_dbg(&dev->dev, "SKX: received FF_DAMPER request s: %d l: %d (LSX: %d, LSY: %d, RSX: %d, RSY: %d)", s, effect->replay.length, ltx, lty, rtx, rty);
      packet->data[0] = 0x09;
      packet->data[1] = 0x00;
      packet->data[2] = skx->data_serial++;
      packet->data[3] = 0x09;
      packet->data[4] = 0x00;
      packet->data[5] = 0x0F;
      packet->data[6] = 0x00; // Left Trigger Strength MIN 00 MAX FF
      packet->data[7] = 0x00; // Right Trigger Strength MIN 00 MAX FF
      packet->data[8] = s; // Heavy Rumble Strength MIN 40 MAX 0x64, off 00
      packet->data[9] = s; // Light Rumble Strength MIN 40 MAX 0x64, off 00
      packet->data[10] = 0x50; // Effect Length MIN 0x00 MAX FF
      packet->data[11] = 0x00; // Break Length MIN 0x00 MAX FF
      packet->data[12] = 0x00; // Number of additional effects  MIN 0x00 MAX FF
      packet->len = 13;
      break;
    default:
      dev_dbg(&dev->dev, "SKX: received unknown FF request\n");
      packet->data[0] = 0x09;
      packet->data[1] = 0x00;
      packet->data[2] = skx->data_serial++;
      packet->data[3] = 0x09;
      packet->data[4] = 0x00;
      packet->data[5] = 0x0F;
      packet->data[6] = 0x00; // Left Trigger Strength MIN 00 MAX FF
      packet->data[7] = 0x00; // Right Trigger Strength MIN 00 MAX FF
      packet->data[8] = 0x00; // Heavy Rumble Strength MIN 40 MAX FF, off 00
      packet->data[9] = 0x00; // Light Rumble Strength MIN 40 MAX FF, off 00
      packet->data[10] = 0x50; // Effect Length MIN 0x00 MAX FF
      packet->data[11] = 0x00; // Break Length MIN 0x00 MAX FF
      packet->data[12] = 0x00; // Number of additional effects  MIN 0x00 MAX FF
      packet->len = 13;

  }


  //memcpy(packet->data, buffer, packet->len);
  packet->is_pending = true;

  err = skx_send_packet(skx);
  if(err)
  {
    dev_dbg(&dev->dev, "SKX: error sending FF packet %d \n", err);
  }

  spin_unlock_irqrestore(&skx->output_data_lock, flags);

  return 0;
}

/*static void skx_delayed_action(struct work_struct *work)
{
  struct my_work *w = container_of(work, struct my_work, wrk);
  struct usb_skx *skx = w->skx;
  struct output_packet *packet = &skx->out_packets[1];
  int i;
  spin_lock_irqsave(&skx->output_data_lock, flags);
  switch(delay_queue[0])
  {
    case(DELAY_FF_SPRING):
      packet->data[0] = 0x09;
      packet->data[1] = 0x00;
      packet->data[2] = skx->data_serial++;
      packet->data[3] = 0x09;
      packet->data[4] = 0x00;
      packet->data[5] = 0x0F;
      packet->data[6] = lT_level; // Left Trigger Strength MIN 00 MAX FF
      packet->data[7] = rT_level; // Right Trigger Strength MIN 00 MAX FF
      packet->data[8] = 0x00; // Heavy Rumble Strength MIN 40 MAX FF, off 00
      packet->data[9] = 0x00; // Light Rumble Strength MIN 40 MAX FF, off 00
      packet->data[10] = 0x10; // Effect Length MIN 0x00 MAX FF
      packet->data[11] = 0x00; // Break Length MIN 0x00 MAX FF
      packet->data[12] = 0x00; // Number of additional effects  MIN 0x00 MAX FF
      packet->len = 13;
      packet->is_pending = true;
      skx_send_packet(skx);
      break;
  }
  spin_unlock_irqrestore(&skx->output_data_lock, flags);
  for (i = 63; i > 1; i--){   
      delay_queue[i]=delay_queue[i-1];
    }
  delay_queue[63] = 0;
}*/

static int skx_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
  struct usb_device *usb_dev = interface_to_usbdev(interface);
  struct usb_endpoint_descriptor *interrupt_in;
  struct usb_skx *skx;
  int err;

  /*skx_workqueue = create_workqueue("skx_workqueue");*/

  if(interface->cur_altsetting->desc.bNumEndpoints != 2)
  {
    return -ENODEV;
  }

  skx = kzalloc(sizeof(struct usb_skx), GFP_KERNEL);
  if(!skx)
  {
    return -ENOMEM;
  }

  usb_make_path(usb_dev, skx->phys_path, sizeof(skx->phys_path));
  strlcat(skx->phys_path, "/input0", sizeof(skx->phys_path));
  dev_dbg(&interface->dev, "Recieved Device Path: %s", skx->phys_path);

  skx->input_data = usb_alloc_coherent(usb_dev, PKT_LEN, GFP_KERNEL,
    &skx->input_data_dma);
  if(!skx->input_data)
  {
    //Should free memory first!
    return -ENOMEM;
  }

  skx->interrupt_in = usb_alloc_urb(0, GFP_KERNEL);
  if(!skx->interrupt_in)
  {
    //Should free memory first!
    return -ENOMEM;
  }

  skx->interface=interface;
  skx->usb_dev=usb_dev;
  skx->name = "Microsoft X-Box One S pad";

  if (interface->cur_altsetting->desc.bInterfaceNumber != 0) {
    //Should free memory first!
    return -ENODEV;
  }

  err = skx_init_output(interface, skx);
  if(err)
  {
    //Should free memory first!
    return -ENOMEM;
  }

  interrupt_in = &interface->cur_altsetting->endpoint[1].desc;

  usb_fill_int_urb(skx->interrupt_in, usb_dev,
    usb_rcvintpipe(usb_dev, interrupt_in->bEndpointAddress),
      skx->input_data, PKT_LEN, skx_interrupt_in,
      skx, interrupt_in->bInterval);

  usb_set_intfdata(interface, skx);

  err = skx_init_input(skx);
  if(err)
  {
    //Should free memory first!
    return -ENOMEM;
  }
  err = skx_start_input(skx);
  if(err)
  {
    //Should free memory first!
    return -ENOMEM;
  }

  input_set_capability(skx->dev, EV_FF, FF_RUMBLE);
  input_set_capability(skx->dev, EV_FF, FF_CONSTANT);
  input_set_capability(skx->dev, EV_FF, FF_SPRING);
  input_set_capability(skx->dev, EV_FF, FF_DAMPER);
  
  err = input_ff_create_memless(skx->dev, NULL, skx_play_ff);
  if (err){
    return err;
  }

  return 0;
}

static void skx_interrupt_in(struct urb *urb)
{
  struct usb_skx *skx = urb->context;
  struct device *d = &skx->interface->dev;
  int err;
  unsigned char *data = skx->input_data;

  //

  err = urb->status;

  switch (err) {
  case 0:
    break;
  case -ECONNRESET:
  case -ENOENT:
  case -ESHUTDOWN:
    dev_dbg(d, "SKX: input urb error: %d\n",  err);
    return;
  default:
    dev_dbg(d, "SKX: input unknown urb status: %d\n", err);
    goto exit;
  }

  //Print this if we need to make sure something works
  //print_hex_dump(KERN_DEBUG, "SKX IN: ", DUMP_PREFIX_OFFSET, 32, 1, skx->input_data, PKT_LEN, 0);

    switch(data[0]) {
      case 0x07:
        if(data[1]==0x30){
          unsigned long flags;
          struct output_packet *packet =
              &skx->out_packets[0];
          static const u8 report_ack[] = {
            0x01, 0x20, 0x00, 0x09, 0x00,
            0x07, 0x20, 0x02, 0x00, 0x00,
            0x00, 0x00, 0x00
          };

          spin_lock_irqsave(&skx->output_data_lock, flags);

          packet->len = sizeof(report_ack);
          memcpy(packet->data, report_ack, packet->len);
          packet->data[2] = data[2];
          packet->is_pending = true;

          /* Reset the sequence so we send out the ack now */
          skx->last_out_packet = -1;
          skx_send_packet(skx);

          spin_unlock_irqrestore(&skx->output_data_lock, flags);
        }
        input_report_key(skx->dev, BTN_MODE, data[4] & 0x01);
        input_sync(skx->dev);
        break;
      case 0x20:
        input_report_key(skx->dev, BTN_START,  data[4] & 0x04);
        input_report_key(skx->dev, BTN_SELECT, data[4] & 0x08);

        /* buttons A,B,X,Y */
        input_report_key(skx->dev, BTN_A,  data[4] & 0x10);
        input_report_key(skx->dev, BTN_B,  data[4] & 0x20);
        input_report_key(skx->dev, BTN_X,  data[4] & 0x40);
        input_report_key(skx->dev, BTN_Y,  data[4] & 0x80);

        /* DPAD Axis */
        input_report_abs(skx->dev, ABS_HAT0X,
             !!(data[5] & 0x08) - !!(data[5] & 0x04));
        input_report_abs(skx->dev, ABS_HAT0Y,
             !!(data[5] & 0x02) - !!(data[5] & 0x01));

        /* Stick Press Buttons */
        input_report_key(skx->dev, BTN_THUMBL, data[5] & 0x40);
        input_report_key(skx->dev, BTN_THUMBR, data[5] & 0x80);

        /* Bumpers */
        input_report_key(skx->dev, BTN_TL, data[5] & 0x10);
        input_report_key(skx->dev, BTN_TR, data[5] & 0x20);

        /* Triggers */
        input_report_abs(skx->dev, ABS_Z,
         (__u16) le16_to_cpup((__le16 *)(data + 6)));
        lT_level = data[6];
        lT_overflow = data[7];
        input_report_abs(skx->dev, ABS_RZ,
         (__u16) le16_to_cpup((__le16 *)(data + 8)));
        rT_level = data[8];
        rT_overflow = data[9];
        /* Left Stick */
        input_report_abs(skx->dev, ABS_X,
             (__s16) le16_to_cpup((__le16 *)(data + 10)));
        lSX_level = data[11];
        input_report_abs(skx->dev, ABS_Y,
             ~(__s16) le16_to_cpup((__le16 *)(data + 12)));
        lSY_level = data[13];

        /* Right Stick */
        input_report_abs(skx->dev, ABS_RX,
             (__s16) le16_to_cpup((__le16 *)(data + 14)));
        rSX_level = data[15];
        input_report_abs(skx->dev, ABS_RY,
             ~(__s16) le16_to_cpup((__le16 *)(data + 16)));
        rSY_level = data[17];
        /*
          All Finished
        */

        if(data[4] & 0x01)
          dev_dbg(d, "Wireless Connect Button pressed.\n");
        if(data[4] & 0x02)
          dev_dbg(d, "Xbox Button pressed.\n");
        if(data[4] & 0x04)
          dev_dbg(d, "Start Button pressed.\n");
        if(data[4] & 0x08)
          dev_dbg(d, "Select Button pressed.\n");
        if(data[4] & 0x10)
          dev_dbg(d, "A Button pressed.\n");
        if(data[4] & 0x20)
          dev_dbg(d, "B Button pressed.\n");
        if(data[4] & 0x40)
          dev_dbg(d, "X Button pressed.\n");
        if(data[4] & 0x80)
          dev_dbg(d, "Y Button pressed.\n");

        if(data[5] & 0x01)
          dev_dbg(d, "Up DPAD pressed.\n");
        if(data[5] & 0x02)
          dev_dbg(d, "Down DPAD pressed.\n");
        if(data[5] & 0x04)
          dev_dbg(d, "Left DPAD pressed.\n");
        if(data[5] & 0x08)
          dev_dbg(d, "Right DPAD pressed.\n");
        if(data[5] & 0x10)
          dev_dbg(d, "Left Bumper pressed.\n");
        if(data[5] & 0x20)
          dev_dbg(d, "Right Bumper pressed.\n");
        if(data[5] & 0x40)
          dev_dbg(d, "Left Stick pressed.\n");
        if(data[5] & 0x80)
          dev_dbg(d, "Right Stick pressed.\n");

        if (data[6] == 0xFF && data[7] == 3) 
          dev_dbg(d, "Left Trigger pressed fully down.\n");
        if (data[8] == 0xFF && data[9] == 3)
          dev_dbg(d, "Right Trigger pressed fully down.\n");
        if(data[11] >= 127 && data[11] < 130)
          dev_dbg(d, "Left Stick pressed fully outwards on X axis.\n");
        if(data[13] >= 127 && data[13] < 130)
          dev_dbg(d, "Left Stick pressed fully outwards on Y axis.\n");
        if(data[15] >= 127 && data[15] < 130)
          dev_dbg(d, "Right Stick pressed fully outwards on X axis.\n");
        if(data[17] >= 127 && data[17] < 130)
          dev_dbg(d, "Right Stick pressed fully outwards on Y axis.\n");
        input_sync(skx->dev);
        break;

        input_sync(skx->dev);
        break;
    }


exit:
  err = usb_submit_urb(urb, GFP_KERNEL);
  if (err){
    dev_err(d, "SKX: input usb_submit_urb failed: %d\n", err);
  }
}

static void skx_interrupt_out(struct urb *urb)
{
  struct usb_skx *skx = urb->context;
  struct device *d = &skx->interface->dev;
  int status = urb->status;
  int err;
  unsigned long flags;

  spin_lock_irqsave(&skx->output_data_lock, flags);

  switch (status) {
  case 0:
    skx->interrupt_out_active = skx_prepare_packet(skx);
    break;

  case -ECONNRESET:
  case -ENOENT:
  case -ESHUTDOWN:
    dev_dbg(d, "SKX: output urb error: %d\n",  err);
    skx->interrupt_out_active = false;
    break;

  default:
    dev_dbg(d, "SKX:output unknown urb status: %d\n", err);
    break;
  }

  //print_hex_dump(KERN_DEBUG, "SKX OUT: ", DUMP_PREFIX_OFFSET, 32, 1, skx->output_data, PKT_LEN, 0);

  if (skx->interrupt_out_active) {
    usb_anchor_urb(urb, &skx->interrupt_out_anchor);
    err = usb_submit_urb(urb, GFP_KERNEL);
    if (err) {
      dev_err(d, "SKX: usb_submit_urb failed: %d\n", err);
      usb_unanchor_urb(urb);
      skx->interrupt_out_active = false;
    }
  }

  spin_unlock_irqrestore(&skx->output_data_lock, flags);
}

static int skx_send_packet(struct usb_skx *skx)
{
  int err;

  if (!skx->interrupt_out_active && skx_prepare_packet(skx)) {
    usb_anchor_urb(skx->interrupt_out, &skx->interrupt_out_anchor);
    err = usb_submit_urb(skx->interrupt_out, GFP_ATOMIC);
    if (err) {
      dev_err(&skx->interface->dev, "SKX: usb_submit_urb failed:%d\n", err);
      usb_unanchor_urb(skx->interrupt_out);
      return -EIO;
    }

    skx->interrupt_out_active = true;
  }

  return 0;
}

static bool skx_prepare_packet(struct usb_skx *skx)
{
  struct output_packet *pkt, *packet = NULL;
  int i;

  for (i = 0; i < MAX_OUT_PACKETS; i++) {
    if (++skx->last_out_packet >= MAX_OUT_PACKETS)
      skx->last_out_packet = 0;

    pkt = &skx->out_packets[skx->last_out_packet];
    if (pkt->is_pending) {
      dev_dbg(&skx->interface->dev,"SKX: found pending output: %d\n", skx->last_out_packet);
      packet = pkt;
      break;
    }
  }

  if (packet) {
    memcpy(skx->output_data, packet->data, packet->len);
    skx->interrupt_out->transfer_buffer_length = packet->len;
    packet->is_pending = false;
    return true;
  }

  return false;
}

static void skx_disconnect(struct usb_interface *interface)
{
  struct usb_skx *skx = usb_get_intfdata(interface);

  usb_kill_urb(skx->interrupt_in);

  input_unregister_device(skx->dev);

  if (!usb_wait_anchor_empty_timeout(&skx->interrupt_out_anchor, 5000)) {
      usb_kill_anchored_urbs(&skx->interrupt_out_anchor);
    }

  usb_free_urb(skx->interrupt_out);
  usb_free_coherent(skx->usb_dev, PKT_LEN,
      skx->output_data, skx->output_data_dma);

  usb_free_urb(skx->interrupt_in);
  usb_free_coherent(skx->usb_dev, PKT_LEN,
      skx->input_data, skx->input_data_dma);

  kfree(skx);

  usb_set_intfdata(interface, NULL);
}
static int skx_init_output(struct usb_interface *interface, struct usb_skx *skx)
{
  struct usb_endpoint_descriptor *interrupt_out;


  init_usb_anchor(&skx->interrupt_out_anchor);

  skx->output_data = usb_alloc_coherent(skx->usb_dev, PKT_LEN, GFP_KERNEL, &skx->output_data_dma);
  if (!skx->output_data) {
    return -ENOMEM;
  }

  spin_lock_init(&skx->output_data_lock);

  skx->interrupt_out = usb_alloc_urb(0, GFP_KERNEL);
  if (!skx->interrupt_out) {
    usb_free_coherent(skx->usb_dev, PKT_LEN, skx->output_data, skx->output_data_dma);
    return -ENOMEM;
  }

  /* Xbox One controller has in/out endpoints swapped. */
  interrupt_out = &interface->cur_altsetting->endpoint[0].desc;

  usb_fill_int_urb(skx->interrupt_out, skx->usb_dev,
       usb_sndintpipe(skx->usb_dev, interrupt_out->bEndpointAddress),
       skx->output_data, PKT_LEN,
       skx_interrupt_out, skx, interrupt_out->bInterval);

  skx->interrupt_out->transfer_dma = skx->output_data_dma;
  skx->interrupt_out->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

  return 0;
}

static int skx_init_input(struct usb_skx *skx)
{
  struct input_dev *indev;
  int i, error;

  indev = input_allocate_device();
  if (!indev)
    return -ENOMEM;
  skx->dev = indev;
  indev->name = skx->name;
  indev->phys = skx->phys_path;
  usb_to_input_id(skx->usb_dev, &indev->id);
  indev->dev.parent = &skx->interface->dev;
  input_set_drvdata(indev, skx);


  __set_bit(EV_KEY, indev->evbit);
  __set_bit(EV_ABS, indev->evbit);
  for (i = 0; skx_buttons[i] >= 0; i++)
      __set_bit(skx_buttons[i], indev->keybit);
  for (i = 0; skx_axis[i] >= 0; i++){
    set_bit(skx_axis[i], indev->absbit);
    switch (skx_axis[i]) {
      case ABS_X:
      case ABS_Y:
      case ABS_RX:
      case ABS_RY:
        input_set_abs_params(indev, skx_axis[i], -32768, 32767, 16, 128);
        break;
      case ABS_Z:
      case ABS_RZ:
        input_set_abs_params(indev, skx_axis[i], 0, 1023, 0, 0);
        break;
      case ABS_HAT0X:
      case ABS_HAT0Y:
        input_set_abs_params(indev, skx_axis[i], -1, 1, 0, 0);
        break;
    }
  }

  error = input_register_device(skx->dev);
  if (error)
  {
    //input_ff_destroy(indev);
    input_free_device(indev);
    return error;
  }

  return 0;
}

static int skx_start_input(struct usb_skx *skx)
{
  int error;
  static const u8 init_pkt_1[] = {
    0x01, 0x20, 0x00, 0x09, 0x00,
    0x04, 0x20, 0x3a, 0x00, 0x00,
    0x00, 0x80, 0x00
  };
  static const u8 init_pkt_2[] = {0x05, 0x20, 0x00, 0x01, 0x00};
  struct output_packet *packet =
      &skx->out_packets[0];
  unsigned long flags;

  if (usb_submit_urb(skx->interrupt_in, GFP_KERNEL))
    return -EIO;

  

  spin_lock_irqsave(&skx->output_data_lock, flags);

  WARN_ON_ONCE(packet->is_pending);

  memcpy(packet->data, init_pkt_1, sizeof(init_pkt_1));
  packet->data[2] = skx->data_serial++;
  packet->len = sizeof(init_pkt_1);
  packet->is_pending = true;

  skx->last_out_packet = -1;
  error = skx_send_packet(skx);

  if (error) {
    usb_kill_urb(skx->interrupt_in);
    return error;
  }

  WARN_ON_ONCE(packet->is_pending);

  memcpy(packet->data, init_pkt_2, sizeof(init_pkt_2));
  packet->data[2] = skx->data_serial++;
  packet->len = sizeof(init_pkt_2);
  packet->is_pending = true;

  skx->last_out_packet = -1;
  error = skx_send_packet(skx);

  if (error) {
    usb_kill_urb(skx->interrupt_in);
    return error;
  }

  spin_unlock_irqrestore(&skx->output_data_lock, flags);

  return 0;
}

static struct usb_driver skx_driver = {
  .name   = "skx",
  .probe    = skx_probe,
  .disconnect = skx_disconnect,
  .id_table = skx_table,
};

module_usb_driver(skx_driver);
