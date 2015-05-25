/*
 *  GPIO IRQ DHT sensor module for AR9331
 *
 *  Copyright (C) 2015 Dmitriy Zherebkov <dzh@black-swift.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/clk.h>

#include <asm/mach-ath79/ar71xx_regs.h>
#include <asm/mach-ath79/ath79.h>
#include <asm/mach-ath79/irq.h>

#include <asm/delay.h>
#include <asm/siginfo.h>

#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>

//#define DEBUG_OUT

#ifdef	DEBUG_OUT
#define debug(fmt,args...)	printk (KERN_INFO fmt ,##args)
#else
#define debug(fmt,args...)
#endif	/* DEBUG_OUT */

//#define SIG_TIMER_IRQ	(SIGRTMIN+12)	// SIGRTMIN is different in Kernel and User modes
#define SIG_DHT_IRQ	44				// So we have to hardcode this value

////////////////////////////////////////////////////////////////////////////////////////////

#define DRV_NAME	"GPIO IRQ DHT"
#define FILE_NAME	"irq-dht"

////////////////////////////////////////////////////////////////////////////////////////////

static unsigned int _timer_frequency=200000000;
static spinlock_t	_lock;

////////////////////////////////////////////////////////////////////////////////////////////

#define ATH79_TIMER0_IRQ		ATH79_MISC_IRQ(0)
#define AR71XX_TIMER0_VALUE		0x00
#define AR71XX_TIMER0_RELOAD	0x04

#define ATH79_TIMER1_IRQ		ATH79_MISC_IRQ(8)
#define AR71XX_TIMER1_VALUE		0x94
#define AR71XX_TIMER1_RELOAD	0x98

#define ATH79_TIMER2_IRQ		ATH79_MISC_IRQ(9)
#define AR71XX_TIMER2_VALUE		0x9C
#define AR71XX_TIMER2_RELOAD	0xA0

#define ATH79_TIMER3_IRQ		ATH79_MISC_IRQ(10)
#define AR71XX_TIMER3_VALUE		0xA4
#define AR71XX_TIMER3_RELOAD	0xA8

static struct _timer_desc_struct
{
	unsigned int	irq;
	unsigned int	value_reg;
	unsigned int	reload_reg;
} _timers[4]=
{
		{	ATH79_TIMER0_IRQ, AR71XX_TIMER0_VALUE, AR71XX_TIMER0_RELOAD	},
		{	ATH79_TIMER1_IRQ, AR71XX_TIMER1_VALUE, AR71XX_TIMER1_RELOAD	},
		{	ATH79_TIMER2_IRQ, AR71XX_TIMER2_VALUE, AR71XX_TIMER2_RELOAD	},
		{	ATH79_TIMER3_IRQ, AR71XX_TIMER3_VALUE, AR71XX_TIMER3_RELOAD	}
};

////////////////////////////////////////////////////////////////////////////////////////////

#define GPIO_OFFS_READ		0x04
#define GPIO_OFFS_SET		0x0C
#define GPIO_OFFS_CLEAR		0x10

////////////////////////////////////////////////////////////////////////////////////////////

void __iomem *ath79_timer_base=NULL;

void __iomem *gpio_addr=NULL;
void __iomem *gpio_readdata_addr=NULL;
void __iomem *gpio_setdataout_addr=NULL;
void __iomem *gpio_cleardataout_addr=NULL;

////////////////////////////////////////////////////////////////////////////////////////////

#define DHT11 11
#define DHT22 22

////////////////////////////////////////////////////////////////////////////////

typedef struct
{
	int				timer;
	int				irq;
	unsigned int	timeout;			//	allways microseconds
} _timer_handler;

static _timer_handler	_thandler;

#define	_max_ticks	200

typedef struct
{
	unsigned int	timeout;
	int				value;
} _gpio_tick;

typedef struct
{
	int				gpio;
	int				irq;
	int				last_value;

	unsigned int	last_time;

	int				counter;
	_gpio_tick		ticks[_max_ticks];
} _gpio_handler;

static _gpio_handler	_ghandler;

static struct dentry* in_file;

////////////////////////////////////////////////////////////////////////////////////////////

static int is_space(char symbol)
{
	return (symbol == ' ') || (symbol == '\t');
}

////////////////////////////////////////////////////////////////////////////////////////////

static int is_digit(char symbol)
{
	return (symbol >= '0') && (symbol <= '9');
}

////////////////////////////////////////////////////////////////////////////////////////////

static irqreturn_t timer_interrupt(int irq, void* dev_id)
{
	//	do nothing
	return (IRQ_HANDLED);
}

////////////////////////////////////////////////////////////////////////////////////////////

static irqreturn_t gpio_edge_interrupt(int irq, void* dev_id)
{
	_gpio_handler* handler=(_gpio_handler*)dev_id;

	if(handler && (handler->irq == irq))
	{
		int val=0;
		unsigned int cur_time=0;

//		debug("Got _handler!\n");

		val=(__raw_readl(gpio_addr + GPIO_OFFS_READ) >> handler->gpio) & 1;

		cur_time=__raw_readl(ath79_timer_base + _timers[_thandler.timer].value_reg)/(_timer_frequency/1000000);

		if(val != handler->last_value)
		{
			handler->last_value=val;

			if(handler->counter == -1)
			{
				handler->last_time=cur_time;
				handler->counter=0;
			}
			else
			{
				if(handler->counter < _max_ticks)
				{
					unsigned int timeout=handler->last_time-cur_time;

					handler->last_time=cur_time;

					handler->ticks[handler->counter].timeout=timeout;
					handler->ticks[handler->counter++].value=!val;
				}
				else
				{
//					stop();
//					free_handler();
				}
			}
		}
	}
	else
	{
		debug("IRQ %d event - no handlers found!\n",irq);
	}

	return (IRQ_HANDLED);
}

////////////////////////////////////////////////////////////////////////////////////////////

static int add_irq(int gpio,void* data)
{
    if(gpio_request(gpio, DRV_NAME) >= 0)
    {
		int irq_number=gpio_to_irq(gpio);

		if(irq_number >= 0)
		{
		    int err = request_irq(irq_number, gpio_edge_interrupt, IRQ_TYPE_EDGE_BOTH, "gpio_irq_handler", data);

		    if(!err)
		    {
		    	debug("Got IRQ %d for GPIO %d\n", irq_number, gpio);
				return irq_number;
		    }
		    else
		    {
		    	debug("GPIO IRQ handler: trouble requesting IRQ %d error %d\n",irq_number, err);
		    }
		}
		else
		{
			debug("Can't map GPIO %d to IRQ : error %d\n",gpio, irq_number);
		}
    }
    else
    {
    	debug("Can't get GPIO %d\n", gpio);
    }

    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////

static void free_handler(void)
{
	_gpio_handler* handler=&_ghandler;

	if(handler->gpio > 0)
	{
		if(handler->irq >= 0)
		{
			free_irq(handler->irq, (void*)handler);
			handler->irq=-1;
		}

		gpio_free(handler->gpio);
		handler->gpio=-1;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////

static int add_handler(int gpio)
{
	_gpio_handler* handler=&_ghandler;

	if(handler->gpio != gpio)
	{
		int irq=add_irq(gpio, handler);

		if(irq < 0)
		{
			free_handler();
			return -1;
		}

		handler->gpio=gpio;
		handler->irq=irq;
		handler->last_value=-1;

		return 0;
	}

	return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////

static int add_timer_irq(int timer, void* data)
{
	int err=0;
	int irq_number=_timers[timer].irq;

	debug("Adding IRQ %d handler\n",irq_number);

	err=request_irq(irq_number, timer_interrupt, 0, DRV_NAME, data);

	if(!err)
	{
		debug("Got IRQ %d.\n", irq_number);
		return irq_number;
	}
	else
	{
		debug("Timer IRQ handler: trouble requesting IRQ %d error %d\n",irq_number, err);
	}

    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////

static void clear_timer(_timer_handler* handler)
{
	handler->timer=-1;
	handler->irq=-1;
	handler->timeout=0;
}

////////////////////////////////////////////////////////////////////////////////////////////

static void stop(void)
{
	unsigned long flags=0;

	_timer_handler* handler=&_thandler;

	spin_lock_irqsave(&_lock,flags);

	if(handler->irq >= 0)
	{
		free_irq(handler->irq, (void*)handler);
		clear_timer(handler);

		debug("Timer stopped.\n");
	}

	spin_unlock_irqrestore(&_lock,flags);
}

////////////////////////////////////////////////////////////////////////////////////////////

static int start(int timer,unsigned int timeout)
{
	int irq=-1;
	unsigned long flags=0;

	_timer_handler* handler=&_thandler;

	stop();

	spin_lock_irqsave(&_lock,flags);
	// need some time (10 ms) before first IRQ - even after "lock"?!
	__raw_writel(_timer_frequency, ath79_timer_base+_timers[timer].reload_reg);

	irq=add_timer_irq(timer,handler);

	if(irq >= 0)
	{
		unsigned int real_timeout=_timer_frequency/1000000*timeout;

/*		int	scale=0;

		scale=timeout/100;
		if(scale >= 2)
		{
			real_timeout/=100;
			handler->ticks_in_timeout=scale;
		}
*/
		handler->timer=timer;
		handler->irq=irq;
		handler->timeout=timeout;

		__raw_writel(real_timeout, ath79_timer_base+_timers[timer].reload_reg);

		debug("Timer #%d started with %u us interval.\n", timer, timeout);

		spin_unlock_irqrestore(&_lock,flags);
		return 0;
	}

	spin_unlock_irqrestore(&_lock,flags);

	stop();
	return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////

static ssize_t run_command(struct file *file, const char __user *buf,
                                size_t count, loff_t *ppos)
{
	char buffer[512];
	char line[20];
	char* in_pos=NULL;
	char* end=NULL;
	char* out_pos=NULL;

	int timer=-1;
	int gpio=-1;
	pid_t pid=0;

	if(count > 512)
		return -EINVAL;	//	file is too big

	copy_from_user(buffer, buf, count);
	buffer[count]=0;

	debug("Command is found (%u bytes length):\n%s\n",count,buffer);

	in_pos=buffer;
	end=in_pos+count-1;

	while(in_pos < end)
	{
		timer=0;
		gpio=-1;

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;	//	skip whitespace
		if(in_pos >= end) break;

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0]))
		{
			sscanf(line, "%u", &timer);
		}
		else
		{
			printk(KERN_INFO "Can't read timer number.\n");
			break;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;	//	skip whitespace

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0]))
		{
			sscanf(line, "%d", &gpio);
		}
		else
		{
			printk(KERN_INFO "Can't read GPIO number.\n");
			break;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;	//	skip whitespace

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0]))
		{
			sscanf(line, "%u", &pid);
		}

		start(timer,1000000);

		add_handler(gpio);

		_ghandler.counter=-1;
		gpio_direction_output(gpio,0);
		__raw_writel(1 << gpio, gpio_cleardataout_addr);
		udelay(2000);
		__raw_writel(1 << gpio, gpio_setdataout_addr);
		udelay(20);
		gpio_direction_input(gpio);

		udelay(100000);

		stop();
		free_handler();

		debug("Total %d values:\n",_ghandler.counter);

/*		{
			int i=0;
			for(; i < _ghandler.counter; ++i)
			{
				printk("%d:%d, ",_ghandler.ticks[i].value,_ghandler.ticks[i].timeout);
			}
			printk("\n");
		}
*/
		if(_ghandler.counter >= 10)
		{
			int i=0;
			int b=0;

			int data[5] = { 0,0,0,0,0 };
			int octet=0;

			while(_ghandler.ticks[i].timeout < 500) ++i;
			if(	(_ghandler.ticks[i].value == 0) &&
//				(_ghandler.ticks[i+1].timeout < 50)	&&	// 1 - differ for DHT11 and DHT22
				(_ghandler.ticks[i+2].timeout > 65)	&&	// 0 - should be ~80
				(_ghandler.ticks[i+3].timeout > 65)) 	// 1 - should be ~80
			{
				i+=4;
				for(; i < _ghandler.counter; ++i)
				{
					if(_ghandler.ticks[i].value)
					{
						if(_ghandler.ticks[i].timeout >= 40)
						{
//							printk("1");

							if(octet < 5)
							{
								data[octet]|=1 << (7-(b % 8));
							}
						}
						else
						{
//							printk("0");
						}

						if((++b % 8) == 0)
						{
							if(octet < 5)
							{
//								printk("=%d",data[octet]);
							}

							++octet;

							if(i < (_ghandler.counter-1))
							{
//								printk(" ");
							}
						}
					}
				}
//				printk("\n");

				{
					int type=((data[1] == 0) && (data[3] == 0))?DHT11:DHT22;

					bool isOK=false;

					debug("type: %d\n",type);

					if(	(((data[0]+data[1]+data[2]+data[3]) & 0xff) == data[4]) &&
						(data[0] || data[1] || data[2] || data[3]))
					 {
						isOK=true;
					 }

					if(isOK)
					 {
						if(pid != 0)
						{
							struct siginfo info;
							struct task_struct* ts=NULL;
							unsigned short t=0;
							unsigned short h=0;

							if(type == DHT11)
							 {
								t=data[2]*10;
								h=data[0]*10;
							 }
							 else
							  {
								t=((data[2] & 0x7f)*256+data[3]);
								h=data[0]*256+data[1];

								if(data[2] & 0x80)
								{
									t|=0x8000;
								}
							  }

							/* send the signal */
							memset(&info, 0, sizeof(struct siginfo));
							info.si_signo = SIG_DHT_IRQ;
							info.si_code = SI_QUEUE;	// this is bit of a trickery: SI_QUEUE is normally used by sigqueue from user space,
											// and kernel space should use SI_KERNEL. But if SI_KERNEL is used the real_time data
											// is not delivered to the user space signal handler function.

							info.si_int=(h << 16) | t;

							rcu_read_lock();
							ts=pid_task(find_vpid(pid), PIDTYPE_PID);
							rcu_read_unlock();

							if(ts)
							{
								send_sig_info(SIG_DHT_IRQ, &info, ts);    //send the signal
								debug("Signal sent to PID %u with parameter 0x%X\n",pid,info.si_int);
							}
							else
							{
								debug("Process with PID %u is not found.\n",pid);
							}
						}
						else
						{
							//just print results
							if(type == DHT11)
							 {
								const char* format="T:%d\tH:%d%%\n";

								printk(format,
											data[2],
											data[0]);
							 }
							 else
							  {
								const char* format="T:%d.%1d\tH:%d.%1d%%\n";

								int t=((data[2] & 0x7f)*256+data[3]);
								int h=data[0]*256+data[1];

								if(data[2] & 0x80)
								{
									t=-t;
								}

								printk(format,
											t/10,t%10,
											h/10,h%10);
							  }
						}

						break;
					 }
				}
			}
		}

		if(pid != 0)
		{
			struct siginfo info;
			struct task_struct* ts=NULL;

			/* send the signal */
			memset(&info, 0, sizeof(struct siginfo));
			info.si_signo = SIG_DHT_IRQ;
			info.si_code = SI_QUEUE;	// this is bit of a trickery: SI_QUEUE is normally used by sigqueue from user space,
							// and kernel space should use SI_KERNEL. But if SI_KERNEL is used the real_time data
							// is not delivered to the user space signal handler function.

			info.si_int=0;	//	means 'error'

			rcu_read_lock();
			ts=pid_task(find_vpid(pid), PIDTYPE_PID);
			rcu_read_unlock();

			if(ts)
			{
				send_sig_info(SIG_DHT_IRQ, &info, ts);    //send the signal
				debug("Error sent to PID %u\n",pid);
			}
			else
			{
				debug("Error, but process with PID %u is not found.\n",pid);
			}
		}
		else
		{
			printk(KERN_INFO "Error.\n");
		}

		break;
	}

	return count;
}

////////////////////////////////////////////////////////////////////////////////////////////

static const struct file_operations irq_fops = {
//	.read = show_handlers,
	.write = run_command,
};

////////////////////////////////////////////////////////////////////////////////////////////

struct clk	//	defined in clock.c
{
	unsigned long rate;
};

////////////////////////////////////////////////////////////////////////////////////////////

static int __init mymodule_init(void)
{
	struct clk* ahb_clk=clk_get(NULL,"ahb");
	if(ahb_clk)
	{
		_timer_frequency=ahb_clk->rate;
	}

	ath79_timer_base = ioremap_nocache(AR71XX_RESET_BASE, AR71XX_RESET_SIZE);

	gpio_addr = ioremap_nocache(AR71XX_GPIO_BASE, AR71XX_GPIO_SIZE);

    gpio_readdata_addr     = gpio_addr + GPIO_OFFS_READ;
    gpio_setdataout_addr   = gpio_addr + GPIO_OFFS_SET;
    gpio_cleardataout_addr = gpio_addr + GPIO_OFFS_CLEAR;

	_ghandler.gpio=-1;
	_ghandler.irq=-1;
	_ghandler.counter=-1;

	clear_timer(&_thandler);

	in_file=debugfs_create_file(FILE_NAME, 0666, NULL, NULL, &irq_fops);

	printk(KERN_INFO "Waiting for commands in file /sys/kernel/debug/" FILE_NAME ".\n");

    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////

static void __exit mymodule_exit(void)
{
	stop();
	free_handler();

	debugfs_remove(in_file);

	return;
}

////////////////////////////////////////////////////////////////////////////////////////////

module_init(mymodule_init);
module_exit(mymodule_exit);

////////////////////////////////////////////////////////////////////////////////////////////

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dmitriy Zherebkov (Black Swift team)");

////////////////////////////////////////////////////////////////////////////////////////////
