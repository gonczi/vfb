/*
 *  vfbts.c -- Virtual framebuffer and touchscreen driver for fb based VNC servers
 *
 *      Copyright (C) 2023 Zoltan Gonczi
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <linux/fb.h>
#include <linux/init.h>

#include <linux/input.h>
#include <linux/input/mt.h>

#define VFB_DRIVER_NAME "vfbts"
#define VFB_DEVHANDLER_NAME "virtual_fbts"
#define VFB_FBDEV_NAME "Virtual FB"
#define VFB_UNIQ_LEN 64
#define VFB_UNIQ_LEN_S "64"		// for sscanf pattern 

#define VFB_TSDEV_NAME "Virtual touchscreen"
    /*
     *  RAM we reserve for the frame buffer. This defines the maximum screen
     *  size
     *
     *  The default can be overridden if the driver is compiled as a module
     */

#define VIDEOMEMSIZE	(1*1024*1024)	/* 1 MB */

static u_long videomemorysize = VIDEOMEMSIZE;
module_param(videomemorysize, ulong, 0);
MODULE_PARM_DESC(videomemorysize, "RAM available to frame buffer (in bytes)");

static char *mode_option = NULL;
module_param(mode_option, charp, 0);
MODULE_PARM_DESC(mode_option, "Preferred video mode (e.g. 640x480-8@60)");

static const struct fb_videomode vfb_default = {
	.xres =		640,
	.yres =		480,
	.pixclock =	20000,
	.left_margin =	64,
	.right_margin =	64,
	.upper_margin =	32,
	.lower_margin =	32,
	.hsync_len =	64,
	.vsync_len =	2,
	.vmode =	FB_VMODE_NONINTERLACED,
};

static struct fb_fix_screeninfo vfb_fix = {
	.id =		VFB_FBDEV_NAME,
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_PSEUDOCOLOR,
	.xpanstep =	1,
	.ypanstep =	1,
	.ywrapstep =	1,
	.accel =	FB_ACCEL_NONE,
};

static int vfb_check_var(struct fb_var_screeninfo *var,
			 struct fb_info *info);
static int vfb_set_par(struct fb_info *info);
static int vfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info);
static int vfb_pan_display(struct fb_var_screeninfo *var,
			   struct fb_info *info);
static int vfb_mmap(struct fb_info *info,
		    struct vm_area_struct *vma);

static const struct fb_ops vfb_ops = {
	.owner		= THIS_MODULE,
	.fb_read        = fb_sys_read,
	.fb_write       = fb_sys_write,
	.fb_check_var	= vfb_check_var,
	.fb_set_par		= vfb_set_par,
	.fb_setcolreg	= vfb_setcolreg,
	.fb_pan_display	= vfb_pan_display,
	.fb_fillrect	= sys_fillrect,
	.fb_copyarea	= sys_copyarea,
	.fb_imageblit	= sys_imageblit,
	.fb_mmap		= vfb_mmap,
};

static int vfb_create_device(const char* uniq);
static void vfb_delete_device(const char* uniq);
static void vfb_get_device_uniq(struct fb_info *fb_info, char* uniq, size_t max_len);
static void vfb_delete_devices(void);

static ssize_t vfb_show_uniq(struct device *device, struct device_attribute *attr, char *buf);
static struct device_attribute vfb_device_attr_uniq = __ATTR(uniq, S_IRUGO, vfb_show_uniq, NULL);

static int vfb_add_device_attr_uniq(struct fb_info *fb_info);
static void vfb_cleanup_device_attr_uniq(struct fb_info *fb_info);

static DEFINE_MUTEX(vfb_device_pool_lock);
#define VFB_DEVICE_POOL_SIZE FB_MAX
struct vfb_device_pool_item {
	int in_use;
	char uniq[VFB_UNIQ_LEN];
	struct platform_device *dev;

	struct input_dev *ts_dev;
};
static struct vfb_device_pool_item vfb_device_pool[VFB_DEVICE_POOL_SIZE];

static int vfb_devhandler_init(void);
static void vfb_devhandler_exit(void);

static int vfb_devhandler_open(struct inode *, struct file *);
static int vfb_devhandler_release(struct inode *, struct file *);
static ssize_t vfb_devhandler_read(struct file *, char *, size_t, loff_t *);
static ssize_t vfb_devhandler_write(struct file *, const char *, size_t, loff_t *);

static int vfb_devhandler_major = 0;    // Major number assigned to our device driver
static int vfb_devhandler_is_open = 0;  // Is device open?  Used to prevent multiple access to the device

struct class * vfb_devhandler_cl = NULL;
struct device * vfb_devhandler_dev = NULL; 

struct file_operations vfb_devhandler_fops __attribute__((__section__(".text"))) = {
	read: vfb_devhandler_read,
	write: vfb_devhandler_write,
	open: vfb_devhandler_open,
	release: vfb_devhandler_release
};

#define ABS_X_MIN	0
#define ABS_X_MAX	1024
#define ABS_Y_MIN	0
#define ABS_Y_MAX	768

#define MAX_CONTACTS 10    // 10 fingers is it

static int virt_ts_init(struct input_dev **p_virt_ts_dev, const char* uniq);
static void virt_ts_unregister(struct input_dev *virt_ts_dev);


    /*
     *  Internal routines
     */

static u_long get_line_length(int xres_virtual, int bpp)
{
	u_long length;

	length = xres_virtual * bpp;
	length = (length + 31) & ~31;
	length >>= 3;
	return (length);
}

    /*
     *  Setting the video mode has been split into two parts.
     *  First part, xxxfb_check_var, must not write anything
     *  to hardware, it should only verify and adjust var.
     *  This means it doesn't alter par but it does use hardware
     *  data from it to check this var.
     */

static int vfb_check_var(struct fb_var_screeninfo *var,
			 struct fb_info *info)
{
	u_long line_length;

	/*
	 *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
	 *  as FB_VMODE_SMOOTH_XPAN is only used internally
	 */

	if (var->vmode & FB_VMODE_CONUPDATE) {
		var->vmode |= FB_VMODE_YWRAP;
		var->xoffset = info->var.xoffset;
		var->yoffset = info->var.yoffset;
	}

	/*
	 *  Some very basic checks
	 */
	if (!var->xres)
		var->xres = 1;
	if (!var->yres)
		var->yres = 1;
	if (var->xres > var->xres_virtual)
		var->xres_virtual = var->xres;
	if (var->yres > var->yres_virtual)
		var->yres_virtual = var->yres;
	if (var->bits_per_pixel <= 1)
		var->bits_per_pixel = 1;
	else if (var->bits_per_pixel <= 8)
		var->bits_per_pixel = 8;
	else if (var->bits_per_pixel <= 16)
		var->bits_per_pixel = 16;
	else if (var->bits_per_pixel <= 24)
		var->bits_per_pixel = 24;
	else if (var->bits_per_pixel <= 32)
		var->bits_per_pixel = 32;
	else
		return -EINVAL;

	if (var->xres_virtual < var->xoffset + var->xres)
		var->xres_virtual = var->xoffset + var->xres;
	if (var->yres_virtual < var->yoffset + var->yres)
		var->yres_virtual = var->yoffset + var->yres;

	/*
	 *  Memory limit
	 */
	line_length =
	    get_line_length(var->xres_virtual, var->bits_per_pixel);
	if (line_length * var->yres_virtual > videomemorysize)
		return -ENOMEM;

	/*
	 * Now that we checked it we alter var. The reason being is that the video
	 * mode passed in might not work but slight changes to it might make it
	 * work. This way we let the user know what is acceptable.
	 */
	switch (var->bits_per_pixel) {
	case 1:
	case 8:
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 0;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 16:		/* RGBA 5551 */
		if (var->transp.length) {
			var->red.offset = 0;
			var->red.length = 5;
			var->green.offset = 5;
			var->green.length = 5;
			var->blue.offset = 10;
			var->blue.length = 5;
			var->transp.offset = 15;
			var->transp.length = 1;
		} else {	/* RGB 565 */
			var->red.offset = 0;
			var->red.length = 5;
			var->green.offset = 5;
			var->green.length = 6;
			var->blue.offset = 11;
			var->blue.length = 5;
			var->transp.offset = 0;
			var->transp.length = 0;
		}
		break;
	case 24:		/* RGB 888 */
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 16;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 32:		/* RGBA 8888 */
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 16;
		var->blue.length = 8;
		var->transp.offset = 24;
		var->transp.length = 8;
		break;
	}
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;

	return 0;
}

/* This routine actually sets the video mode. It's in here where we
 * the hardware state info->par and fix which can be affected by the
 * change in par. For this driver it doesn't do much.
 */
static int vfb_set_par(struct fb_info *info)
{
	switch (info->var.bits_per_pixel) {
	case 1:
		info->fix.visual = FB_VISUAL_MONO01;
		break;
	case 8:
		info->fix.visual = FB_VISUAL_PSEUDOCOLOR;
		break;
	case 16:
	case 24:
	case 32:
		info->fix.visual = FB_VISUAL_TRUECOLOR;
		break;
	}

	info->fix.line_length = get_line_length(info->var.xres_virtual,
						info->var.bits_per_pixel);

	return 0;
}

    /*
     *  Set a single color register. The values supplied are already
     *  rounded down to the hardware's capabilities (according to the
     *  entries in the var structure). Return != 0 for invalid regno.
     */

static int vfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info)
{
	if (regno >= 256)	/* no. of hw registers */
		return 1;
	/*
	 * Program hardware... do anything you want with transp
	 */

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
		    (red * 77 + green * 151 + blue * 28) >> 8;
	}

	/* Directcolor:
	 *   var->{color}.offset contains start of bitfield
	 *   var->{color}.length contains length of bitfield
	 *   {hardwarespecific} contains width of RAMDAC
	 *   cmap[X] is programmed to (X << red.offset) | (X << green.offset) | (X << blue.offset)
	 *   RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Pseudocolor:
	 *    var->{color}.offset is 0 unless the palette index takes less than
	 *                        bits_per_pixel bits and is stored in the upper
	 *                        bits of the pixel value
	 *    var->{color}.length is set so that 1 << length is the number of available
	 *                        palette entries
	 *    cmap is not used
	 *    RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Truecolor:
	 *    does not use DAC. Usually 3 are present.
	 *    var->{color}.offset contains start of bitfield
	 *    var->{color}.length contains length of bitfield
	 *    cmap is programmed to (red << red.offset) | (green << green.offset) |
	 *                      (blue << blue.offset) | (transp << transp.offset)
	 *    RAMDAC does not exist
	 */
#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		red = CNVT_TOHW(red, 8);	/* expect 8 bit DAC */
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		u32 v;

		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
		switch (info->var.bits_per_pixel) {
		case 8:
			break;
		case 16:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		case 24:
		case 32:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		}
		return 0;
	}
	return 0;
}

    /*
     *  Pan or Wrap the Display
     *
     *  This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
     */

static int vfb_pan_display(struct fb_var_screeninfo *var,
			   struct fb_info *info)
{
	if (var->vmode & FB_VMODE_YWRAP) {
		if (var->yoffset >= info->var.yres_virtual ||
		    var->xoffset)
			return -EINVAL;
	} else {
		if (var->xoffset + info->var.xres > info->var.xres_virtual ||
		    var->yoffset + info->var.yres > info->var.yres_virtual)
			return -EINVAL;
	}
	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;
	if (var->vmode & FB_VMODE_YWRAP)
		info->var.vmode |= FB_VMODE_YWRAP;
	else
		info->var.vmode &= ~FB_VMODE_YWRAP;
	return 0;
}

    /*
     *  Most drivers don't need their own mmap function
     */

static int vfb_mmap(struct fb_info *info,
		    struct vm_area_struct *vma)
{
	return remap_vmalloc_range(vma, (void *)info->fix.smem_start, vma->vm_pgoff);
}

    /*
     *  Initialisation
     */

static int vfb_probe(struct platform_device *dev)
{
	void *videomemory;
	struct fb_info *info;
	unsigned int size = PAGE_ALIGN(videomemorysize);
	int retval = -ENOMEM;

	printk("vfb_probe\n");

	/*
	 * For real video cards we use ioremap.
	 */
	if (!(videomemory = vmalloc_32_user(size)))
		return retval;

	info = framebuffer_alloc(sizeof(u32) * 256, &dev->dev);
	if (!info)
		goto err;

	info->screen_buffer = videomemory;
	info->fbops = &vfb_ops;

	if (!fb_find_mode(&info->var, info, mode_option,
			  NULL, 0, &vfb_default, 8)){
		fb_err(info, "Unable to find usable video mode.\n");
		retval = -EINVAL;
		goto err1;
	}

	info->fix = vfb_fix;
	info->fix.smem_start = (unsigned long) videomemory;
	info->fix.smem_len = videomemorysize;

	info->pseudo_palette = info->par;
	info->par = NULL;

	retval = fb_alloc_cmap(&info->cmap, 256, 0);
	if (retval < 0)
		goto err1;

	retval = register_framebuffer(info);
	if (retval < 0)
		goto err2;
	
	platform_set_drvdata(dev, info);

	vfb_add_device_attr_uniq(info);

	vfb_set_par(info);

	fb_info(info, "Virtual frame buffer device, using %ldK of video memory\n",
		videomemorysize >> 10);
	return 0;
err2:
	fb_dealloc_cmap(&info->cmap);
err1:
	framebuffer_release(info);
err:
	vfree(videomemory);
	return retval;
}

static void vfb_remove(struct platform_device *dev)
{
	void *videomemory;
	struct fb_info *info = platform_get_drvdata(dev);

	printk("vfb_remove\n");

	if (info) {
		videomemory = info->screen_buffer;
		vfb_cleanup_device_attr_uniq(info);
		unregister_framebuffer(info);
		vfree(videomemory);
		fb_dealloc_cmap(&info->cmap);
		framebuffer_release(info);
	}
}

static struct platform_driver vfb_driver = {
	.probe	= vfb_probe,
	.remove_new = vfb_remove,
	.driver = {
		.name	= VFB_DRIVER_NAME,
	},
};

static int vfb_create_device(const char* uniq)
{
	int ret;
	int pdpidx = -1;
	bool uniq_already_exists = false;

	printk("vfb_create_device\n");

	//------------------------------------------
	mutex_lock(&vfb_device_pool_lock);
	
	uniq_already_exists = false;
	for (int i = 0; i < VFB_DEVICE_POOL_SIZE; i++) {
		if (vfb_device_pool[i].in_use) {
			if (0 == strncmp(vfb_device_pool[i].uniq, uniq, sizeof(vfb_device_pool[i].uniq) - 1)) {
				uniq_already_exists = true;
				break;
			}
		}
	}
	
	if (false == uniq_already_exists) {
		for (int i = 0; i < VFB_DEVICE_POOL_SIZE; i++) {
			if (!vfb_device_pool[i].in_use) {
				vfb_device_pool[i].in_use = 1;
				strncpy(vfb_device_pool[i].uniq, uniq, sizeof(vfb_device_pool[i].uniq)); // --> /sys/class/graphics/fb*/uniq
				pdpidx = i;
				break;
			}
		}
	} 

	mutex_unlock(&vfb_device_pool_lock);
	//------------------------------------------

	if (true == uniq_already_exists) {
		printk("vfb_create_device: device uniq (%s) already exists\n", uniq);
		return -EINVAL;
	}

	if (pdpidx == -1) {
		printk("vfb_create_device: can't alloc more device\n");
		return -ENOMEM;
	}

	printk("vfb_create_device: pdpidx[%d]\n", pdpidx);
	vfb_device_pool[pdpidx].dev = platform_device_alloc(VFB_DRIVER_NAME, pdpidx);

	if (vfb_device_pool[pdpidx].dev) {
		ret = platform_device_add(vfb_device_pool[pdpidx].dev);
	} else {
		ret = -ENOMEM;
	}

	if (ret) {
		platform_device_put(vfb_device_pool[pdpidx].dev);
		vfb_device_pool[pdpidx].in_use = 0;
	} else {

		vfb_device_pool[pdpidx].ts_dev = NULL;
	 	ret = virt_ts_init(&(vfb_device_pool[pdpidx].ts_dev), vfb_device_pool[pdpidx].uniq);
	}

	return ret;
}

void vfb_delete_device(const char* uniq)
{
	printk("vfb_delete_device [%s]\n", uniq);

	for (int i = 0; i < VFB_DEVICE_POOL_SIZE; i++) {
		struct platform_device *dev = NULL;
		struct input_dev *ts_dev = NULL;

		mutex_lock(&vfb_device_pool_lock);
		if (vfb_device_pool[i].in_use
			&& (0 == strncmp(vfb_device_pool[i].uniq, uniq, sizeof(vfb_device_pool[i].uniq) - 1))) {
			
			dev = vfb_device_pool[i].dev;
			ts_dev = vfb_device_pool[i].ts_dev;
			vfb_device_pool[i].in_use = 0;
			vfb_device_pool[i].ts_dev = NULL;
			vfb_device_pool[i].dev = NULL;
		}
		mutex_unlock(&vfb_device_pool_lock);

		if (ts_dev) {
			printk("vfb_delete_device: virt_ts_unregister[%d]\n", i);
			virt_ts_unregister(ts_dev);
		}

		if (dev) {
			printk("vfb_delete_device: platform_device_unregister[%d]\n", i);
			platform_device_unregister(dev);

			return;
		}
	}

	printk("vfb_delete_device: device not found\n");
}

static void vfb_get_device_uniq(struct fb_info *fb_info, char* uniq, size_t max_len)
{
	mutex_lock(&vfb_device_pool_lock);
	for (int i = 0; i < VFB_DEVICE_POOL_SIZE; i++) {
		if (vfb_device_pool[i].in_use) {
			if ((struct fb_info *)platform_get_drvdata(vfb_device_pool[i].dev) == fb_info) {
				strncpy(uniq, vfb_device_pool[i].uniq, max_len);
				break;
			}
		}
	}
	mutex_unlock(&vfb_device_pool_lock);
}

static void vfb_delete_devices(void)
{
	printk("vfb_delete_devices\n");

	for (int i = 0; i < VFB_DEVICE_POOL_SIZE; i++) {
		struct platform_device *dev = NULL;

		mutex_lock(&vfb_device_pool_lock);
		if (vfb_device_pool[i].in_use) {
			dev = vfb_device_pool[i].dev;
			vfb_device_pool[i].in_use = 0;
			vfb_device_pool[i].dev = NULL;
		}
		mutex_unlock(&vfb_device_pool_lock);

		if (dev) {
			printk("vfb_delete_devices: platform_device_unregister[%d]\n", i);
			platform_device_unregister(dev);
		}
	}
}



static ssize_t vfb_show_uniq(struct device *device,
			 struct device_attribute *attr, char *buf)
{
	char uniq_buff[VFB_UNIQ_LEN] = "";
	struct fb_info *fb_info = dev_get_drvdata(device);

	vfb_get_device_uniq(fb_info, uniq_buff, sizeof(uniq_buff));

	return sysfs_emit(buf, "%s\n", uniq_buff);
}

static int vfb_add_device_attr_uniq(struct fb_info *fb_info)
{
	device_create_file(fb_info->dev, &vfb_device_attr_uniq);
	return 0;
}

static void vfb_cleanup_device_attr_uniq(struct fb_info *fb_info)
{
	device_remove_file(fb_info->dev, &vfb_device_attr_uniq);
}




static int vfb_devhandler_open(struct inode *inode, struct file *file) 
{
    if (vfb_devhandler_is_open) return -EBUSY;
    try_module_get(THIS_MODULE);
    ++vfb_devhandler_is_open;
    return 0;
}
	
static int vfb_devhandler_release(struct inode *inode, struct file *file) 
{
    --vfb_devhandler_is_open;
    module_put(THIS_MODULE);
    return 0;
}

static ssize_t vfb_devhandler_read(struct file *filp, char *buffer, size_t length, loff_t *offset) 
{
    const char* message = 
        "Usage: write the following commands to /dev/" VFB_DEVHANDLER_NAME ":\n"
        "    add <UUID>  - add new fb device\n"
        "    del <UUID>  - delete fb device\n";
    const size_t msgsize = strlen(message);
    loff_t off = *offset;

    if (off >= msgsize) {
        return 0;
    }

    if (length > msgsize - off) {
        length = msgsize - off;
    }

    if (copy_to_user(buffer, message + off, length) != 0) {
        return -EFAULT;
    }

    *offset += length;

    return length;
}
	
static void vfb_devhandler_execute_command(const char *cmd, const char* name)
{
    if (0 == strncmp(cmd, "add", 3)) {
		vfb_create_device(name);
    } else if (0 == strncmp(cmd, "del", 3)) {
		vfb_delete_device(name);
    } else {
		printk("<4>" VFB_DEVHANDLER_NAME ": Unknown command<%s> with ID<%s>\n", cmd, name);
    }
}

static ssize_t vfb_devhandler_write(struct file *filp, const char *ubuf, size_t len, loff_t *off)
{
    char cmd[4] = {0};
    char vfb_uniq[VFB_UNIQ_LEN] = {0};

    char buf[VFB_UNIQ_LEN * 2];
    size_t len_to_use = len;
    size_t i;
    size_t p = 0;

    if (len_to_use > sizeof(buf)) {
		len_to_use = sizeof(buf);
	}

    if (copy_from_user(buf, ubuf, len_to_use) != 0) {
        return -EFAULT;
    }

    for (i = 0; i < len_to_use; ++i) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
            if (sscanf(buf + p, "%4s %" VFB_UNIQ_LEN_S "[^\n]", cmd, vfb_uniq) != 2) {
                printk("<4>" VFB_DEVHANDLER_NAME ": sscanf failed to interpret this input\n");
            }
            p = i + 1;

			vfb_devhandler_execute_command(cmd, vfb_uniq);
        }
    }

    if (p == 0 && len != 0) {
        printk("<4>" VFB_DEVHANDLER_NAME ": Command incomplete or too long. Trailing \\n is required.\n");
        // prevent endless loop
        return len;
    }

    return p;
}

int vfb_devhandler_init(void)
{
    int res = 0;

    vfb_devhandler_major = register_chrdev(0, VFB_DEVHANDLER_NAME, &vfb_devhandler_fops);	
    if (vfb_devhandler_major < 0) {
	    printk("Registering the character device failed with %d\n", vfb_devhandler_major);
        res = vfb_devhandler_major;
	} else {
		printk("" VFB_DEVHANDLER_NAME ": vfb_devhandler_major=%d\n", vfb_devhandler_major);
		// vfb_devhandler_cl = class_create(THIS_MODULE, VFB_DEVHANDLER_NAME);
		vfb_devhandler_cl = class_create(VFB_DEVHANDLER_NAME);
		if (!IS_ERR(vfb_devhandler_cl)) {
			vfb_devhandler_dev = device_create(vfb_devhandler_cl, NULL, MKDEV(vfb_devhandler_major, 0), NULL, VFB_DEVHANDLER_NAME);
		}
	}

    return res;
}

void vfb_devhandler_exit(void)
{
    if (!IS_ERR(vfb_devhandler_cl)) {
	    device_destroy(vfb_devhandler_cl, MKDEV(vfb_devhandler_major, 0));
	    class_destroy(vfb_devhandler_cl);
    }

    unregister_chrdev(vfb_devhandler_major, VFB_DEVHANDLER_NAME);
}

// --------------------------------------------------------------------------------------

static int virt_ts_init(struct input_dev **p_virt_ts_dev, const char* uniq)
{
	int err;
	struct input_dev *virt_ts_dev;

	virt_ts_dev = (*p_virt_ts_dev) = input_allocate_device();
	if (!virt_ts_dev)
		return -ENOMEM;

	virt_ts_dev->evbit[0] = BIT_MASK(EV_ABS) | BIT_MASK(EV_KEY);
	virt_ts_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	input_set_abs_params(virt_ts_dev, ABS_X, ABS_X_MIN, ABS_X_MAX, 0, 0);
	input_set_abs_params(virt_ts_dev, ABS_Y, ABS_Y_MIN, ABS_Y_MAX, 0, 0);

	virt_ts_dev->name = VFB_TSDEV_NAME;
	virt_ts_dev->uniq = uniq;

    input_mt_init_slots(virt_ts_dev, MAX_CONTACTS, INPUT_MT_DIRECT);

	input_set_abs_params(virt_ts_dev, ABS_MT_POSITION_X, ABS_X_MIN, ABS_X_MAX, 0, 0);
	input_set_abs_params(virt_ts_dev, ABS_MT_POSITION_Y, ABS_Y_MIN, ABS_Y_MAX, 0, 0);

	err = input_register_device(virt_ts_dev);
	if (!err) {
		return 0;
	}

	input_free_device(virt_ts_dev);
 	(*p_virt_ts_dev) = NULL;
	return err;
}

static void virt_ts_unregister(struct input_dev *virt_ts_dev)
{
	input_unregister_device(virt_ts_dev);
}

// --------------------------------------------------------------------------------------

static int __init vfb_init(void)
{
	int ret = 0;

	printk("vfb_init\n");

	memset(&vfb_device_pool, 0, sizeof(vfb_device_pool));

	ret = platform_driver_register(&vfb_driver);

	if (!ret) {
		vfb_devhandler_init();
	}

	return ret;
}

static void __exit vfb_exit(void)
{
	printk("vfb_exit\n");

	vfb_devhandler_exit();

	vfb_delete_devices();
	platform_driver_unregister(&vfb_driver);
}

module_init(vfb_init);
module_exit(vfb_exit);

MODULE_AUTHOR("Zoltan Gonczi, zoltan.gonczi@gmail.com");
MODULE_DESCRIPTION("Virtual framebuffer and touchscreen driver for fb based VNC servers");
MODULE_LICENSE("GPL");
