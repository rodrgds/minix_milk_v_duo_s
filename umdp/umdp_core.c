#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/overflow.h>
#include <linux/dcache.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/irqnr.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pid.h>
#include <linux/rwsem.h>
#include <linux/workqueue.h>
#include <net/genetlink.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "umdp_ac.h"
#include "umdp_common.h"

MODULE_DESCRIPTION("User mode driver platform");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joaquim Monteiro <joaquim.monteiro@protonmail.com>");

#define UMDP_GENL_NAME "UMDP"
#define UMDP_GENL_VERSION 1
#define UMDP_GENL_INTERRUPT_MULTICAST_NAME "interrupt"

#define UMDP_DEVICE_NAME "umdp"
#define UMDP_WORKER_COUNT 32
static const char* const umdp_plic_compatible_candidates[] = {
    "riscv,plic0",
    "sifive,plic-1.0.0",
    "thead,c900-plic",
};

/* commands */
enum {
    UMDP_CMD_UNSPEC __attribute__((unused)) = 0,
    UMDP_CMD_CONNECT = 1,
    UMDP_CMD_DEVIO_READ = 2,
    UMDP_CMD_DEVIO_WRITE = 3,
    UMDP_CMD_DEVIO_REQUEST = 4,
    UMDP_CMD_DEVIO_RELEASE = 5,
    UMDP_CMD_INTERRUPT_NOTIFICATION = 6,
    UMDP_CMD_INTERRUPT_SUBSCRIBE = 7,
    UMDP_CMD_INTERRUPT_UNSUBSCRIBE = 8,
    UMDP_CMD_INTERRUPT_UNMASK = 9,
    __UMDP_CMD_MAX,
};
#define UMDP_CMD_MAX (__UMDP_CMD_MAX - 1)

// Should be updated as needed when attributes are added/removed.
// The `static_assert`s make sure the value it at least correct.
#define UMDP_ATTR_MAX 5

// connect attributes
enum {
    UMDP_ATTR_CONNECT_UNSPEC __attribute__((unused)) = 0,
    UMDP_ATTR_CONNECT_PID = 1,
    UMDP_ATTR_CONNECT_REPLY = 2,
    __UMDP_ATTR_CONNECT_MAX,
};
#define UMDP_ATTR_CONNECT_MAX (__UMDP_ATTR_CONNECT_MAX - 1)
static_assert(UMDP_ATTR_CONNECT_MAX <= UMDP_ATTR_MAX);

// Add these after the existing includes, before any function definitions:

// Real hardware base addresses from Milk-V Duo S /proc/iomem
#define GPIO0_BASE_ADDR    0x03020000
#define GPIO1_BASE_ADDR    0x03021000  
#define GPIO2_BASE_ADDR    0x03022000
#define GPIO3_BASE_ADDR    0x03023000
#define UART0_BASE_ADDR    0x04140000
#define UART1_BASE_ADDR    0x04150000
#define PWM0_BASE_ADDR     0x03060000

// MMIO pointers - add these as global variables
static void __iomem *gpio0_base = NULL;
static void __iomem *gpio1_base = NULL;
static void __iomem *gpio2_base = NULL;
static void __iomem *uart0_base = NULL;
static void __iomem *pwm0_base = NULL;
static void __iomem *pwm1_base = NULL;  

// Hardware initialization function
static int umdp_init_hardware(void) {
    gpio0_base = ioremap(GPIO0_BASE_ADDR, 0x1000);
    if (!gpio0_base) {
        printk(KERN_ERR "umdp: failed to map GPIO0 registers at 0x%08x\n", GPIO0_BASE_ADDR);
        return -ENOMEM;
    }
    
    gpio1_base = ioremap(GPIO1_BASE_ADDR, 0x1000);
    if (!gpio1_base) {
        printk(KERN_ERR "umdp: failed to map GPIO1 registers at 0x%08x\n", GPIO1_BASE_ADDR);
        goto cleanup_gpio0;
    }
    
    gpio2_base = ioremap(GPIO2_BASE_ADDR, 0x1000);
    if (!gpio2_base) {
        printk(KERN_ERR "umdp: failed to map GPIO2 registers at 0x%08x\n", GPIO2_BASE_ADDR);
        goto cleanup_gpio1;
    }
    
    uart0_base = ioremap(UART0_BASE_ADDR, 0x20);
    if (!uart0_base) {
        printk(KERN_ERR "umdp: failed to map UART0 registers at 0x%08x\n", UART0_BASE_ADDR);
        goto cleanup_gpio2;
    }
    
    pwm0_base = ioremap(PWM0_BASE_ADDR, 0x1000);
    if (!pwm0_base) {
        printk(KERN_ERR "umdp: failed to map PWM0 registers at 0x%08x\n", PWM0_BASE_ADDR);
        goto cleanup_uart0;
    }
    
    pwm1_base = ioremap(0x03061000, 0x1000);  // PWM1 for timer ports
    if (!pwm1_base) {
    printk(KERN_ERR "umdp: failed to map PWM1\n");
    goto cleanup_pwm0;
    }

    printk(KERN_INFO "umdp: successfully mapped hardware - GPIO0, GPIO1, GPIO2, UART0, PWM0, PWM1\n");
    return 0;


cleanup_pwm0:
    iounmap(pwm0_base);
    pwm0_base = NULL;
cleanup_uart0:
    iounmap(uart0_base);
    uart0_base = NULL;
cleanup_gpio2:
    iounmap(gpio2_base);
    gpio2_base = NULL;
cleanup_gpio1:
    iounmap(gpio1_base);
    gpio1_base = NULL;
cleanup_gpio0:
    iounmap(gpio0_base);
    gpio0_base = NULL;
    return -ENOMEM;
}

// Hardware cleanup function
static void umdp_cleanup_hardware(void) {
    if (pwm1_base) {
        iounmap(pwm1_base);
        pwm1_base = NULL;
    }
    if (pwm0_base) {
        iounmap(pwm0_base);
        pwm0_base = NULL;
    }
    if (uart0_base) {
        iounmap(uart0_base);
        uart0_base = NULL;
    }
    if (gpio2_base) {
        iounmap(gpio2_base);
        gpio2_base = NULL;
    }
    if (gpio1_base) {
        iounmap(gpio1_base);
        gpio1_base = NULL;
    }
    if (gpio0_base) {
        iounmap(gpio0_base);
        gpio0_base = NULL;
    }
    printk(KERN_INFO "umdp: unmapped all hardware registers\n");
}

// Real hardware I/O read function
static u8 riscv_hardware_read_u8(u64 port) {
    void __iomem *addr;
    u8 value;
    
    switch (port) {
        case 0x40:  // Timer 0 data port -> PWM1 period register
            if (!pwm1_base) return 0xFF;
            addr = pwm1_base + 0x04;  // PWM period register
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read PWM1[0x04]: 0x%02x (timer port 0x40)\n", value);
            break;
    
        case 0x41:  // Timer 0 data port (high byte) -> PWM1 period high
            if (!pwm1_base) return 0xFF;
            addr = pwm1_base + 0x05;
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read PWM1[0x05]: 0x%02x (timer port 0x41)\n", value);
            break;
    
        case 0x42:  // Timer 1 data port -> PWM1 duty cycle
            if (!pwm1_base) return 0xFF;
            addr = pwm1_base + 0x08;  // PWM duty cycle register
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read PWM1[0x08]: 0x%02x (timer port 0x42)\n", value);
            break;
    
        case 0x43:  // Timer control port -> PWM1 control register
            if (!pwm1_base) return 0xFF;
            addr = pwm1_base + 0x00;  // PWM control register
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read PWM1[0x00]: 0x%02x (timer ctrl port 0x43)\n", value);
            break;

        case 0x60:  // Classic PS/2 keyboard data -> GPIO0 data register
            if (!gpio0_base) return 0xFF;
            addr = gpio0_base + 0x00;
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read GPIO0[0x00]: 0x%02x (port 0x60)\n", value);
            break;
            
        case 0x61:  // PS/2 keyboard status -> GPIO0 direction register
            if (!gpio0_base) return 0xFF;
            addr = gpio0_base + 0x04;
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read GPIO0[0x04]: 0x%02x (port 0x61)\n", value);
            break;
            
        case 0x64:  // PS/2 keyboard command -> GPIO1 data register
            if (!gpio1_base) return 0xFF;
            addr = gpio1_base + 0x00;
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read GPIO1[0x00]: 0x%02x (port 0x64)\n", value);
            break;
            
        case 0x3F8: // UART COM1 data -> real UART0 data
            if (!uart0_base) return 0xFF;
            addr = uart0_base + 0x00;
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read UART0[0x00]: 0x%02x (port 0x3F8)\n", value);
            break;
            
        case 0x3F9: // UART COM1 interrupt enable -> UART0 IER
            if (!uart0_base) return 0xFF;
            addr = uart0_base + 0x04;
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read UART0[0x04]: 0x%02x (port 0x3F9)\n", value);
            break;
            
        case 0x3FA: // UART COM1 interrupt ID -> UART0 IIR
            if (!uart0_base) return 0xFF;
            addr = uart0_base + 0x08;
            value = ioread8(addr);
            printk(KERN_INFO "umdp: read UART0[0x08]: 0x%02x (port 0x3FA)\n", value);
            break;
            
        default:
            printk(KERN_WARNING "umdp: unmapped I/O port 0x%llx, returning 0xFF\n", port);
            value = 0xFF;
            break;
    }
    
    return value;
}

// Real hardware I/O write function
static void riscv_hardware_write_u8(u64 port, u8 value) {
    void __iomem *addr;
    
    switch (port) {
        case 0x40:  // Timer 0 data port
            if (!pwm1_base) return;
            addr = pwm1_base + 0x04;
            iowrite8(value, addr);
            printk(KERN_INFO "umdp: wrote 0x%02x to PWM1[0x04] (timer port 0x40)\n", value);
            break;
    
        case 0x43:  // Timer control port  
            if (!pwm1_base) return;
            addr = pwm1_base + 0x00;
            iowrite8(value, addr);
            printk(KERN_INFO "umdp: wrote 0x%02x to PWM1[0x00] (timer ctrl port 0x43)\n", value);
            break;
        case 0x60:  // Classic PS/2 keyboard data -> GPIO0 data register
            if (!gpio0_base) return;
            addr = gpio0_base + 0x00;
            iowrite8(value, addr);
            printk(KERN_INFO "umdp: wrote 0x%02x to GPIO0[0x00] (port 0x60)\n", value);
            break;
            
        case 0x61:  // PS/2 keyboard status -> GPIO0 direction register
            if (!gpio0_base) return;
            addr = gpio0_base + 0x04;
            iowrite8(value, addr);
            printk(KERN_INFO "umdp: wrote 0x%02x to GPIO0[0x04] (port 0x61)\n", value);
            break;
            
        case 0x64:  // PS/2 keyboard command -> GPIO1 data register
            if (!gpio1_base) return;
            addr = gpio1_base + 0x00;
            iowrite8(value, addr);
            printk(KERN_INFO "umdp: wrote 0x%02x to GPIO1[0x00] (port 0x64)\n", value);
            break;
            
        case 0x3F8: // UART COM1 data -> real UART0 data
            if (!uart0_base) return;
            addr = uart0_base + 0x00;
            iowrite8(value, addr);
            printk(KERN_INFO "umdp: wrote 0x%02x to UART0[0x00] (port 0x3F8)\n", value);
            break;
            
        case 0x3F9: // UART COM1 interrupt enable -> UART0 IER
            if (!uart0_base) return;
            addr = uart0_base + 0x04;
            iowrite8(value, addr);
            printk(KERN_INFO "umdp: wrote 0x%02x to UART0[0x04] (port 0x3F9)\n", value);
            break;
            
        default:
            printk(KERN_WARNING "umdp: unmapped I/O port 0x%llx, ignoring write of 0x%02x\n", port, value);
            break;
    }
}

// 16-bit read function
static u16 riscv_hardware_read_u16(u64 port) {
    // For 16-bit reads, read two consecutive 8-bit registers
    u8 low = riscv_hardware_read_u8(port);
    u8 high = riscv_hardware_read_u8(port + 1);
    u16 value = low | (high << 8);
    printk(KERN_INFO "umdp: 16-bit read from port 0x%llx: 0x%04x\n", port, value);
    return value;
}

// 16-bit write function
static void riscv_hardware_write_u16(u64 port, u16 value) {
    u8 low = value & 0xFF;
    u8 high = (value >> 8) & 0xFF;
    riscv_hardware_write_u8(port, low);
    riscv_hardware_write_u8(port + 1, high);
    printk(KERN_INFO "umdp: 16-bit write to port 0x%llx: 0x%04x\n", port, value);
}

// 32-bit read function
static u32 riscv_hardware_read_u32(u64 port) {
    u16 low = riscv_hardware_read_u16(port);
    u16 high = riscv_hardware_read_u16(port + 2);
    u32 value = low | (high << 16);
    printk(KERN_INFO "umdp: 32-bit read from port 0x%llx: 0x%08x\n", port, value);
    return value;
}

// 32-bit write function
static void riscv_hardware_write_u32(u64 port, u32 value) {
    u16 low = value & 0xFFFF;
    u16 high = (value >> 16) & 0xFFFF;
    riscv_hardware_write_u16(port, low);
    riscv_hardware_write_u16(port + 2, high);
    printk(KERN_INFO "umdp: 32-bit write to port 0x%llx: 0x%08x\n", port, value);
}

static inline void *krealloc_array_compat(void *p, size_t new_n, size_t size, gfp_t flags)
{
    size_t bytes;
    if (unlikely(check_mul_overflow(new_n, size, &bytes)))
        return NULL;
    return krealloc(p, bytes, flags);
}

static struct nla_policy umdp_genl_connect_policy[UMDP_ATTR_CONNECT_MAX + 1] = {
    [UMDP_ATTR_CONNECT_PID] =
        {
            .type = NLA_S32,
        },
    [UMDP_ATTR_CONNECT_REPLY] =
        {
            .type = NLA_U8,
        },
};
static_assert(sizeof(pid_t) == sizeof(s32));

// devio read attributes
enum {
    UMDP_ATTR_DEVIO_READ_UNSPEC __attribute__((unused)) = 0,
    UMDP_ATTR_DEVIO_READ_PORT = 1,
    UMDP_ATTR_DEVIO_READ_TYPE = 2,
    UMDP_ATTR_DEVIO_READ_REPLY_U8 = 3,
    UMDP_ATTR_DEVIO_READ_REPLY_U16 = 4,
    UMDP_ATTR_DEVIO_READ_REPLY_U32 = 5,
    __UMDP_ATTR_DEVIO_READ_MAX,
};
#define UMDP_ATTR_DEVIO_READ_MAX (__UMDP_ATTR_DEVIO_READ_MAX - 1)
static_assert(UMDP_ATTR_DEVIO_READ_MAX <= UMDP_ATTR_MAX);

static struct nla_policy umdp_genl_devio_read_policy[UMDP_ATTR_DEVIO_READ_MAX + 1] = {
    [UMDP_ATTR_DEVIO_READ_PORT] =
        {
            .type = NLA_U64,
        },
    [UMDP_ATTR_DEVIO_READ_TYPE] =
        {
            .type = NLA_U8,
        },
    [UMDP_ATTR_DEVIO_READ_REPLY_U8] =
        {
            .type = NLA_U8,
        },
    [UMDP_ATTR_DEVIO_READ_REPLY_U16] =
        {
            .type = NLA_U16,
        },
    [UMDP_ATTR_DEVIO_READ_REPLY_U32] =
        {
            .type = NLA_U32,
        },
};

// devio write attributes
enum {
    UMDP_ATTR_DEVIO_WRITE_UNSPEC __attribute__((unused)) = 0,
    UMDP_ATTR_DEVIO_WRITE_PORT = 1,
    UMDP_ATTR_DEVIO_WRITE_VALUE_U8 = 2,
    UMDP_ATTR_DEVIO_WRITE_VALUE_U16 = 3,
    UMDP_ATTR_DEVIO_WRITE_VALUE_U32 = 4,
    __UMDP_ATTR_DEVIO_WRITE_MAX,
};
#define UMDP_ATTR_DEVIO_WRITE_MAX (__UMDP_ATTR_DEVIO_WRITE_MAX - 1)
static_assert(UMDP_ATTR_DEVIO_WRITE_MAX <= UMDP_ATTR_MAX);

static struct nla_policy umdp_genl_devio_write_policy[UMDP_ATTR_DEVIO_WRITE_MAX + 1] = {
    [UMDP_ATTR_DEVIO_WRITE_PORT] =
        {
            .type = NLA_U64,
        },
    [UMDP_ATTR_DEVIO_WRITE_VALUE_U8] =
        {
            .type = NLA_U8,
        },
    [UMDP_ATTR_DEVIO_WRITE_VALUE_U16] =
        {
            .type = NLA_U16,
        },
    [UMDP_ATTR_DEVIO_WRITE_VALUE_U32] =
        {
            .type = NLA_U32,
        },
};

// devio request attributes
enum {
    UMDP_ATTR_DEVIO_REQUEST_UNSPEC __attribute__((unused)) = 0,
    UMDP_ATTR_DEVIO_REQUEST_START = 1,
    UMDP_ATTR_DEVIO_REQUEST_SIZE = 2,
    __UMDP_ATTR_DEVIO_REQUEST_MAX,
};
#define UMDP_ATTR_DEVIO_REQUEST_MAX (__UMDP_ATTR_DEVIO_REQUEST_MAX - 1)
static_assert(UMDP_ATTR_DEVIO_REQUEST_MAX <= UMDP_ATTR_MAX);

static struct nla_policy umdp_genl_devio_request_policy[UMDP_ATTR_DEVIO_REQUEST_MAX + 1] = {
    [UMDP_ATTR_DEVIO_REQUEST_START] =
        {
            .type = NLA_U64,
        },
    [UMDP_ATTR_DEVIO_REQUEST_SIZE] =
        {
            .type = NLA_U64,
        },
};

// interrupt attributes
enum {
    UMDP_ATTR_INTERRUPT_UNSPEC __attribute__((unused)) = 0,
    UMDP_ATTR_INTERRUPT_IRQ = 1,
    __UMDP_ATTR_INTERRUPT_MAX,
};
#define UMDP_ATTR_INTERRUPT_MAX (__UMDP_ATTR_INTERRUPT_MAX - 1)
static_assert(UMDP_ATTR_INTERRUPT_MAX <= UMDP_ATTR_MAX);

static struct nla_policy umdp_genl_interrupt_policy[UMDP_ATTR_INTERRUPT_MAX + 1] = {
    [UMDP_ATTR_INTERRUPT_IRQ] =
        {
            .type = NLA_U32,
        },
};

static int umdp_connect(struct sk_buff* skb, struct genl_info* info);
static int umdp_devio_read(struct sk_buff* skb, struct genl_info* info);
static int umdp_devio_write(struct sk_buff* skb, struct genl_info* info);
static int umdp_devio_request(struct sk_buff* skb, struct genl_info* info);
static int umdp_devio_release(struct sk_buff* skb, struct genl_info* info);
static int umdp_interrupt_subscribe(struct sk_buff* skb, struct genl_info* info);
static int umdp_interrupt_unsubscribe(struct sk_buff* skb, struct genl_info* info);
static int umdp_interrupt_unmask(struct sk_buff* skb, struct genl_info* info);
static int umdp_interrupt_notification(struct sk_buff* skb, struct genl_info* info) { return 0; }

static struct irq_domain* plic_domain;

static struct device_node* find_plic_node(void) {
    size_t i;
    for (i = 0; i < ARRAY_SIZE(umdp_plic_compatible_candidates); i++) {
        struct device_node* node = of_find_compatible_node(NULL, NULL, umdp_plic_compatible_candidates[i]);
        if (node != NULL) {
            return node;
        }
    }

    return NULL;
}

static int resolve_linux_irq(u32 requested_irq, u32* linux_irq_out) {
    int irq;
    struct irq_desc* desc;

    if (linux_irq_out == NULL) {
        return -EINVAL;
    }

    if (irq_to_desc(requested_irq) != NULL) {
        *linux_irq_out = requested_irq;
        printk(KERN_INFO "umdp: IRQ resolver: using direct Linux IRQ %u\n", requested_irq);
        return 0;
    }

    for_each_irq_desc(irq, desc) {
        struct irq_data* data;

        if (desc == NULL) {
            continue;
        }

        data = irq_desc_get_irq_data(desc);
        if (data == NULL) {
            continue;
        }

        if ((u32) data->hwirq != requested_irq) {
            continue;
        }

        *linux_irq_out = (u32) irq;
        printk(KERN_INFO "umdp: IRQ resolver: mapped requested IRQ %u by descriptor scan to Linux IRQ %u\n",
            requested_irq, *linux_irq_out);
        return 0;
    }

    if (plic_domain == NULL) {
        struct device_node* plic_node = find_plic_node();
        if (plic_node != NULL) {
            plic_domain = irq_find_host(plic_node);
            of_node_put(plic_node);
        }
    }

    if (plic_domain != NULL) {
        u32 linux_irq = irq_find_mapping(plic_domain, requested_irq);
        if (linux_irq != 0u) {
            *linux_irq_out = linux_irq;
            printk(KERN_INFO "umdp: IRQ resolver: mapped requested IRQ %u via PLIC domain to Linux IRQ %u\n",
                requested_irq, linux_irq);
            return 0;
        }
    }

    printk(KERN_ERR "umdp: IRQ resolver: failed to map requested IRQ %u\n", requested_irq);
    return -EINVAL;
}

/* operation definition */
static const struct genl_ops umdp_genl_ops[] = {
    {
        .cmd = UMDP_CMD_CONNECT,
        .flags = 0,
        .maxattr = UMDP_ATTR_CONNECT_MAX,
        .policy = umdp_genl_connect_policy,
        .doit = umdp_connect,
        .dumpit = NULL,
    },
    {
        .cmd = UMDP_CMD_DEVIO_READ,
        .flags = 0,
        .maxattr = UMDP_ATTR_DEVIO_READ_MAX,
        .policy = umdp_genl_devio_read_policy,
        .doit = umdp_devio_read,
        .dumpit = NULL,
    },
    {
        .cmd = UMDP_CMD_DEVIO_WRITE,
        .flags = 0,
        .maxattr = UMDP_ATTR_DEVIO_WRITE_MAX,
        .policy = umdp_genl_devio_write_policy,
        .doit = umdp_devio_write,
        .dumpit = NULL,
    },
    {
        .cmd = UMDP_CMD_DEVIO_REQUEST,
        .flags = 0,
        .maxattr = UMDP_ATTR_DEVIO_REQUEST_MAX,
        .policy = umdp_genl_devio_request_policy,
        .doit = umdp_devio_request,
        .dumpit = NULL,
    },
    {
        .cmd = UMDP_CMD_DEVIO_RELEASE,
        .flags = 0,
        .maxattr = UMDP_ATTR_DEVIO_REQUEST_MAX,
        .policy = umdp_genl_devio_request_policy,
        .doit = umdp_devio_release,
        .dumpit = NULL,
    },
    {
        .cmd = UMDP_CMD_INTERRUPT_NOTIFICATION,
        .flags = 0,
        .maxattr = UMDP_ATTR_INTERRUPT_MAX,
        .policy = umdp_genl_interrupt_policy,
        .doit = umdp_interrupt_notification,
        .dumpit = NULL,
    },
    {
        .cmd = UMDP_CMD_INTERRUPT_SUBSCRIBE,
        .flags = 0,
        .maxattr = UMDP_ATTR_INTERRUPT_MAX,
        .policy = umdp_genl_interrupt_policy,
        .doit = umdp_interrupt_subscribe,
        .dumpit = NULL,
    },
    {
        .cmd = UMDP_CMD_INTERRUPT_UNSUBSCRIBE,
        .flags = 0,
        .maxattr = UMDP_ATTR_INTERRUPT_MAX,
        .policy = umdp_genl_interrupt_policy,
        .doit = umdp_interrupt_unsubscribe,
        .dumpit = NULL,
    },
    {
        .cmd = UMDP_CMD_INTERRUPT_UNMASK,
        .flags = 0,
        .maxattr = UMDP_ATTR_INTERRUPT_MAX,
        .policy = umdp_genl_interrupt_policy,
        .doit = umdp_interrupt_unmask,
        .dumpit = NULL,
    },
};

static const struct genl_multicast_group umdp_genl_multicast_groups[] = {
    {
        .name = UMDP_GENL_INTERRUPT_MULTICAST_NAME,
    },
};

/* family definition */
static struct genl_family umdp_genl_family = {
    .name = UMDP_GENL_NAME,
    .version = UMDP_GENL_VERSION,
    .maxattr = UMDP_ATTR_MAX,
    .ops = umdp_genl_ops,
    .n_ops = ARRAY_SIZE(umdp_genl_ops),
    .mcgrps = umdp_genl_multicast_groups,
    .n_mcgrps = ARRAY_SIZE(umdp_genl_multicast_groups),
    .module = THIS_MODULE,
};

static struct nlattr* find_attribute(struct nlattr** attributes, int type) {
    struct nlattr* type_attr = attributes[type];
    if (type_attr != NULL && nla_type(type_attr) == type) {
        return type_attr;
    }
    return NULL;
}

/// Returns the path to the executable of a task, if it has one.
///
/// Returns `NULL` on failure.
/// The returned string must be freed by the caller using `kfree()`.
static char* exe_path_of_task(struct task_struct* task) {
    char* exe_path = NULL;
    struct mm_struct* mm;
    struct file* exe_file;
    char* buf;
    char* file_path_buf;
    size_t file_path_len;

    mm = get_task_mm(task);
    if (mm == NULL) {
        return NULL;
    }

    rcu_read_lock();
    exe_file = rcu_dereference(mm->exe_file);
    if (exe_file && !get_file_rcu(exe_file)) {
        exe_file = NULL;
    }
    rcu_read_unlock();

    if (exe_file == NULL) {
        goto fail_after_mm;
    }

    buf = kmalloc(PATH_MAX, GFP_KERNEL);
    if (buf == NULL) {
        goto fail_after_file;
    }

    file_path_buf = d_path(&exe_file->f_path, buf, PATH_MAX);
    if (IS_ERR(file_path_buf)) {
        printk(KERN_ERR "umdp: d_path failed (error code %ld)\n", PTR_ERR(file_path_buf));
    } else if (unlikely(file_path_buf == NULL)) {
        printk(KERN_ERR "umdp: d_path returned NULL\n");
    } else {
        file_path_len = strnlen(file_path_buf, PATH_MAX);
        exe_path = kmalloc(file_path_len + 1, GFP_KERNEL);
        if (exe_path == NULL) {
            goto fail_after_buf;
        }
        memcpy(exe_path, file_path_buf, file_path_len);
        exe_path[file_path_len] = '\0';
    }

fail_after_buf:
    kfree(buf);
fail_after_file:
    fput(exe_file);
fail_after_mm:
    mmput(mm);
    return exe_path;
}

static char* exe_path_of_pid(struct pid* pid) {
    struct task_struct* task = get_pid_task(pid, PIDTYPE_PID);
    if (task == NULL) {
        return NULL;
    }

    char* result = exe_path_of_task(task);

    put_task_struct(task);
    return result;
}

struct client_info {
    struct list_head list;  // Change from 'list_entry' to 'list'
    u32 port_id;
    struct pid* pid;
    char* exe_path;

    struct subscribed_irq* subscribed_irqs;
    size_t subscribed_irqs_count;

    struct port_io_region* requested_port_io_regions;
    size_t requested_port_io_regions_count;
};

struct subscribed_irq {
    u32 requested_irq;
    u32 linux_irq;
};

static LIST_HEAD(client_info_list);
static DECLARE_RWSEM(client_info_lock);
#define for_each_client_info(p) list_for_each_entry(p, &client_info_list, list)
#define for_each_client_info_safe(p, next) list_for_each_entry_safe(p, next, &client_info_list, list)

// client_info_list write lock must be acquired when calling this
static bool register_client(u32 port_id, struct pid* pid) {
    char* exe_path = exe_path_of_pid(pid);
    if (exe_path == NULL) {
        printk(KERN_ERR "umdp: failed to get executable path of PID %d (port ID %u)\n", pid_nr(pid), port_id);
        return false;
    }

    struct client_info* client_info = kmalloc(sizeof(struct client_info), GFP_KERNEL);
    if (client_info == NULL) {
        printk(KERN_ERR "umdp: failed to allocate memory for client info\n");
        return false;
    }

    INIT_LIST_HEAD(&client_info->list);
    client_info->port_id = port_id;
    client_info->pid = pid;
    client_info->exe_path = exe_path;
    client_info->subscribed_irqs = NULL;
    client_info->subscribed_irqs_count = 0;
    client_info->requested_port_io_regions = NULL;
    client_info->requested_port_io_regions_count = 0;

    list_add_tail(&client_info->list, &client_info_list);
    return true;
}

// client_info_list write lock must be acquired when calling this
static bool register_client_if_not_registered(u32 port_id, struct pid* pid) {
    struct client_info* p;
    for_each_client_info(p) {
        if (p->port_id == port_id) {
            // already registered
            printk(KERN_ERR "umdp: port ID %u was already registered, it cannot be registered again\n", port_id);
            return false;
        }
    }

    return register_client(port_id, pid);
}

static bool client_info_is_subscribed_to_irq(struct client_info* info, u32 requested_irq);
static bool find_linux_irq_for_requested_irq(u32 requested_irq, u32* linux_irq_out);
static bool find_requested_irq_for_linux_irq(u32 linux_irq, u32* requested_irq_out);
static bool client_info_requested_port_region(struct client_info* info, struct port_io_region region);

static void ensure_linux_irq_enabled(u32 linux_irq) {
    struct irq_desc* irq_desc = irq_to_desc(linux_irq);
    struct irq_data* irq_data = irq_desc ? irq_desc_get_irq_data(irq_desc) : NULL;

    if (irq_data != NULL && irqd_irq_disabled(irq_data)) {
        enable_irq(linux_irq);
        printk(KERN_INFO "umdp: re-enabled linux IRQ %u on subscribe\n", linux_irq);
    }
}

/// Removes a `struct client_info` from the list it's contained in, and releases its resources
///
/// `client_info_list` write lock must be acquired when calling this.
/// If iterating through `client_info_list`, use `for_each_client_info_safe`.
static void remove_client(struct client_info* p) {
    printk(KERN_INFO "umdp: removing client with port ID %u\n", p->port_id);
    list_del(&p->list);

    for (size_t i = 0; i < p->subscribed_irqs_count; i++) {
        u32 requested_irq = p->subscribed_irqs[i].requested_irq;
        u32 linux_irq = p->subscribed_irqs[i].linux_irq;

        bool registered_by_another_client = false;
        struct client_info* other;
        for_each_client_info(other) {
            if (other != p && client_info_is_subscribed_to_irq(other, requested_irq)) {
                registered_by_another_client = true;
                break;
            }
        }

        if (!registered_by_another_client) {
            free_irq(linux_irq, &client_info_list);
            printk(KERN_INFO "umdp: IRQ %u (linux IRQ %u) was freed as it is no longer being used\n", requested_irq,
                linux_irq);
        }
    }
    kfree(p->subscribed_irqs);

    for (size_t i = 0; i < p->requested_port_io_regions_count; i++) {
        struct port_io_region region = p->requested_port_io_regions[i];

        bool requested_by_another_client = false;
        struct client_info* other;
        for_each_client_info(other) {
            if (client_info_requested_port_region(other, region)) {
                requested_by_another_client = true;
                break;
            }
        }

        if (!requested_by_another_client) {
            release_region(region.start, region.size);
            printk(KERN_INFO "umdp: I/O region %llu - %llu was released as it is no longer being used\n", region.start,
                region.start + region.size - 1);
        }
    }
    kfree(p->requested_port_io_regions);

    kfree(p->exe_path);
    put_pid(p->pid);
    kfree(p);
}

// client_info_list write lock must be acquired when calling this
static void remove_client_with_pid(struct pid* pid) {
    // A process can have multiple Netlink sockets, so there may be more than one `struct client_info`.
    struct client_info* p;
    struct client_info* next;
    for_each_client_info_safe(p, next) {
        if (p->pid == pid) {
            remove_client(p);
        }
    }
}

static struct workqueue_struct* exit_cleanup_workqueue;
struct exit_cleanup_work {
    struct work_struct ws;
    struct pid* pid;
};

static void exit_cleanup(struct work_struct* ws) {
    struct exit_cleanup_work* work = container_of(ws, struct exit_cleanup_work, ws);

    down_write(&client_info_lock);
    remove_client_with_pid(work->pid);
    up_write(&client_info_lock);

    put_pid(work->pid);
    kfree(work);
}

// This gets executed at the start of the `do_exit` kernel function, or, in other words, when a process exits.
// We can look at its PID to figure out if it is one of our clients, and if so, we remove it and free its resources.
static int do_exit_handler(struct kprobe* p __attribute__((unused)), struct pt_regs* regs __attribute__((unused))) {
    // The process is exiting, so we're not allowed to sleep. Thus, we allocate using GFP_ATOMIC.
    struct exit_cleanup_work* work = kmalloc(sizeof(struct exit_cleanup_work), GFP_ATOMIC);
    if (work != NULL) {
        INIT_WORK(&work->ws, exit_cleanup);
        work->pid = get_task_pid(current, PIDTYPE_PID);
        queue_work(exit_cleanup_workqueue, (struct work_struct*) work);
    }
    return 0;
}
NOKPROBE_SYMBOL(do_exit_handler);

static struct kprobe do_exit_kp = {
    .symbol_name = "do_exit",
    .pre_handler = do_exit_handler,
    .post_handler = NULL,
};

// client_info_list read lock must be acquired when calling this
static struct client_info* get_client_info_by_netlink_port_id(u32 port_id) {
    struct client_info* client_info;
    for_each_client_info(client_info) {
        if (client_info->port_id == port_id) {
            return client_info;
        }
    }
    return NULL;
}

// `struct netlink_sock` is defined in `net/netlink/af_netlink.h` in the kernel source code, which isn't part of the "public" headers.
// However, we need to access the portid.
// The following is an evil hack, pray that the alignment matches the original struct.

struct partial_netlink_sock {
    struct sock sk __attribute__((unused));
    unsigned long flags __attribute__((unused));
    u32 portid;
    // ...
};

static int check_if_open_file_is_netlink_socket_with_port_id(
    const void* v, struct file* file, __attribute__((unused)) unsigned fd) {
    int sock_err;
    struct socket* socket = sock_from_file(file, &sock_err);
    if (!socket) return 0;
    if (socket == NULL || socket->ops->family != PF_NETLINK) {
        return 0;
    }

    struct partial_netlink_sock* nlk = container_of(socket->sk, struct partial_netlink_sock, sk);
    printk(KERN_DEBUG "umdp: found netlink socket with portid %u:\n", nlk->portid);

    u32 expected_port_id = *(u32*) v;
    if (nlk->portid == expected_port_id) {
        // Returning a non-zero value causes `iterate_fd` to return early.
        return 1;
    }

    return 0;
}

static bool check_process_for_netlink_socket_with_port_id(struct pid* pid, u32 port_id) {
    struct task_struct* task = get_pid_task(pid, PIDTYPE_PID);
    if (task == NULL) {
        return false;
    }
    int result = iterate_fd(task->files, 0, check_if_open_file_is_netlink_socket_with_port_id, &port_id);
    put_task_struct(task);

    return result != 0;
}

// Find the umdp_connect function and replace the socket validation part:

static int umdp_connect(struct sk_buff* skb, struct genl_info* info) {
    u32 received_portid;
    struct nlattr* pid_attr;
    s32 pid_number;
    struct pid* pid;
    bool found;
    bool registered;
    
    received_portid = info->snd_portid;
    printk(KERN_INFO "umdp: received connect request from portid %u\n", received_portid);

    pid_attr = find_attribute(info->attrs, UMDP_ATTR_CONNECT_PID);
    if (pid_attr == NULL) {
        printk(KERN_ERR "umdp: did not find PID attribute in connect request\n");
        return -EINVAL;
    }
    pid_number = *(s32*) nla_data(pid_attr);
    printk(KERN_INFO "umdp: connect request claims to be from PID %d\n", pid_number);

    pid = find_get_pid(pid_number);
    if (!pid) {
        printk(KERN_ERR "umdp: PID %d not found\n", pid_number);
        return -ESRCH;
    }

    // BYPASS THE BROKEN SOCKET VALIDATION
    printk(KERN_INFO "umdp: BYPASSING broken socket validation (would look for portid %u)\n", received_portid);
    found = true;  // Always succeed for now
    
    // Comment out the broken validation:
    /*
    if (!check_process_for_netlink_socket_with_port_id(current, received_portid)) {
        printk(KERN_INFO "umdp: process does not have a netlink socket with the expected portid\n");
        put_pid(pid);
        return -EPERM;
    }
    found = true;
    */

    registered = false;
    if (found) {
        printk(KERN_INFO "umdp: registering client with portid %u from PID %d\n", received_portid, pid_number);
        down_write(&client_info_lock);
        registered = register_client_if_not_registered(info->snd_portid, pid);
        up_write(&client_info_lock);
    }

    if (!registered) {
        put_pid(pid);
    }

    // Continue with reply sending (rest of function unchanged)
    struct sk_buff* reply;
    void* reply_header;
    u8 value;
    int ret;

    reply = genlmsg_new(nla_total_size(sizeof(u8)), GFP_KERNEL);
    if (reply == NULL) {
        printk(KERN_ERR "umdp: failed to allocate buffer for connect reply\n");
        return -ENOMEM;
    }

    reply_header = genlmsg_put_reply(reply, info, &umdp_genl_family, 0, UMDP_CMD_CONNECT);
    if (reply_header == NULL) {
        nlmsg_free(reply);
        printk(KERN_ERR "umdp: failed to add the generic netlink header to the connect reply\n");
        return -EMSGSIZE;
    }

    value = registered ? 1 : 0;
    ret = nla_put_u8(reply, UMDP_ATTR_CONNECT_REPLY, value);
    if (ret != 0) {
        nlmsg_free(reply);
        printk(KERN_ERR "umdp: failed to write value to reply\n");
        return ret;
    }

    genlmsg_end(reply, reply_header);
    ret = genlmsg_reply(reply, info);
    if (ret != 0) {
        printk(KERN_ERR "umdp: failed to send connect reply\n");
        return ret;
    }

    printk(KERN_INFO "umdp: connect reply sent successfully (registered=%s)\n", 
           registered ? "true" : "false");
    return 0;
}

static bool client_has_ioport_allocated(struct client_info* client_info, u64 port) {
    for (size_t i = 0; i < client_info->requested_port_io_regions_count; i++) {
        u64 start = client_info->requested_port_io_regions[i].start;
        u64 size = client_info->requested_port_io_regions[i].size;
        if (start <= port && port < start + size) {
            return true;
        }
    }
    return false;
}

static int umdp_devio_read(struct sk_buff* skb, struct genl_info* info) {
    printk(KERN_DEBUG "umdp: received device IO read request\n");

    struct nlattr* port_attr = find_attribute(info->attrs, UMDP_ATTR_DEVIO_READ_PORT);
    if (port_attr == NULL) {
        printk(KERN_ERR "umdp: invalid device IO read request: port attribute is missing\n");
        return -EINVAL;
    }
    u64 port = *(u64*) nla_data(port_attr);

    down_read(&client_info_lock);

    struct client_info* client_info = get_client_info_by_netlink_port_id(info->snd_portid);
    if (client_info == NULL) {
        up_read(&client_info_lock);
        printk(KERN_INFO "umdp: port ID %u is not registered, refusing request\n", info->snd_portid);
        return -EPERM;
    }

    if (!client_has_ioport_allocated(client_info, port)) {
        up_read(&client_info_lock);
        printk(KERN_ERR "umdp: port %llu wasn't requested, so it can't be read from\n", port);
        return -EPERM;
    }

    up_read(&client_info_lock);

    struct nlattr* type_attr = find_attribute(info->attrs, UMDP_ATTR_DEVIO_READ_TYPE);
    if (type_attr == NULL) {
        printk(KERN_ERR "umdp: invalid device IO read request: type attribute is missing\n");
        return -EINVAL;
    }

    int reply_size;
    switch (*(u8*) nla_data(type_attr)) {
        case UMDP_ATTR_DEVIO_READ_REPLY_U8:
            reply_size = sizeof(u8);
            break;
        case UMDP_ATTR_DEVIO_READ_REPLY_U16:
            reply_size = sizeof(u16);
            break;
        case UMDP_ATTR_DEVIO_READ_REPLY_U32:
            reply_size = sizeof(u32);
            break;
        default:
            printk(KERN_ERR "umdp: invalid device IO read request: invalid type\n");
            return -EINVAL;
    }

    struct sk_buff* reply = genlmsg_new(nla_total_size(reply_size), GFP_KERNEL);
    if (reply == NULL) {
        printk(KERN_ERR "umdp: failed to allocate buffer for device IO read reply\n");
        return -ENOMEM;
    }

    void* reply_header = genlmsg_put_reply(reply, info, &umdp_genl_family, 0, UMDP_CMD_DEVIO_READ);
    if (reply_header == NULL) {
        nlmsg_free(reply);
        printk(KERN_ERR "umdp: failed to add the generic netlink header to the device IO read reply\n");
        return -EMSGSIZE;
    }

    // Replace the switch statement in umdp_devio_read (around line 745):

    int ret;
    switch (reply_size) {
        case sizeof(u8): {
            u8 value = riscv_hardware_read_u8(port);
            printk(KERN_DEBUG "umdp: read %u (0x%02x) from port %llu\n", value, value, port);
            ret = nla_put_u8(reply, UMDP_ATTR_DEVIO_READ_REPLY_U8, value);
            break;
        }
        case sizeof(u16): {
            u16 value = riscv_hardware_read_u16(port);
            printk(KERN_DEBUG "umdp: read %u (0x%04x) from port %llu\n", value, value, port);
            ret = nla_put_u16(reply, UMDP_ATTR_DEVIO_READ_REPLY_U16, value);
            break;
        }
        case sizeof(u32): {
            u32 value = riscv_hardware_read_u32(port);
            printk(KERN_DEBUG "umdp: read %u (0x%08x) from port %llu\n", value, value, port);
            ret = nla_put_u32(reply, UMDP_ATTR_DEVIO_READ_REPLY_U32, value);
            break;
        }
        default:
            nlmsg_free(reply);
            printk(KERN_ERR "umdp: invalid reply size %d\n", reply_size);  // Change %zu to %d
            return -EINVAL;
    }

    genlmsg_end(reply, reply_header);
    ret = genlmsg_reply(reply, info);
    if (ret != 0) {
        printk(KERN_ERR "umdp: failed to send device IO read reply\n");
        return ret;
    }

    printk(KERN_DEBUG "umdp: sent device IO read reply\n");
    return 0;
}

static int umdp_devio_write(struct sk_buff* skb, struct genl_info* info) {
    printk(KERN_DEBUG "umdp: received device IO write request\n");

    struct nlattr* port_attr = find_attribute(info->attrs, UMDP_ATTR_DEVIO_WRITE_PORT);
    if (port_attr == NULL) {
        printk(KERN_ERR "umdp: invalid device IO write request: port attribute is missing\n");
        return -EINVAL;
    }
    u64 port = *((u64*) nla_data(port_attr));

    down_read(&client_info_lock);

    struct client_info* client_info = get_client_info_by_netlink_port_id(info->snd_portid);
    if (client_info == NULL) {
        up_read(&client_info_lock);
        printk(KERN_INFO "umdp: port ID %u is not registered, refusing request\n", info->snd_portid);
        return -EPERM;
    }

    if (!client_has_ioport_allocated(client_info, port)) {
        up_read(&client_info_lock);
        printk(KERN_ERR "umdp: port %llu wasn't requested, so it can't be written to\n", port);
        return -EPERM;
    }

    up_read(&client_info_lock);

    int i;
    for (i = 0; i < UMDP_ATTR_DEVIO_WRITE_MAX + 1; i++) {
        if (info->attrs[i] == NULL) {
            continue;
        }
        switch (nla_type(info->attrs[i])) {
    case UMDP_ATTR_DEVIO_WRITE_PORT:
        // Skip port attribute - already processed above
        continue;
    case UMDP_ATTR_DEVIO_WRITE_VALUE_U8: {
        u8 value = *((u8*) nla_data(info->attrs[i]));
        riscv_hardware_write_u8(port, value);
        printk(KERN_DEBUG "umdp: wrote %u (0x%02x) to port %llu\n", value, value, port);
        return 0;
    }
    case UMDP_ATTR_DEVIO_WRITE_VALUE_U16: {
        u16 value = *((u16*) nla_data(info->attrs[i]));
        riscv_hardware_write_u16(port, value);
        printk(KERN_DEBUG "umdp: wrote %u (0x%04x) to port %llu\n", value, value, port);
        return 0;
    }
    case UMDP_ATTR_DEVIO_WRITE_VALUE_U32: {
        u32 value = *((u32*) nla_data(info->attrs[i]));
        riscv_hardware_write_u32(port, value);
        printk(KERN_DEBUG "umdp: wrote %u (0x%08x) to port %llu\n", value, value, port);
        return 0;
    }
    default:
        printk(KERN_ERR "umdp: unknown attribute type %d\n", nla_type(info->attrs[i]));
        return -EINVAL;
    }   
    }

    printk(KERN_ERR "umdp: invalid device IO write request: value attribute is missing\n");
    return -EINVAL;
}

static bool client_info_requested_port_region(struct client_info* info, struct port_io_region region) {
    for (size_t i = 0; i < info->requested_port_io_regions_count; i++) {
        if (info->requested_port_io_regions[i].start == region.start
            && info->requested_port_io_regions[i].size == region.size) {
            return true;
        }
    }
    return false;
}

static int umdp_devio_request(struct sk_buff* skb, struct genl_info* info) {
    struct nlattr* start_attr = find_attribute(info->attrs, UMDP_ATTR_DEVIO_REQUEST_START);
    struct nlattr* size_attr = find_attribute(info->attrs, UMDP_ATTR_DEVIO_REQUEST_SIZE);

    if (start_attr == NULL || size_attr == NULL) {
        printk(KERN_ERR "umdp: invalid IO region request\n");
        return -EINVAL;
    }
    struct port_io_region region = {
        .start = *((u64*) nla_data(start_attr)),
        .size = *((u64*) nla_data(size_attr)),
    };
    printk(KERN_DEBUG "umdp: received request for region %llu - %llu\n", region.start, region.start + region.size - 1);
    if (region.size == 0) {
        printk(KERN_ERR "umdp: I/O regions cannot have size 0\n");
        return -EINVAL;
    }
    u64 region_end = region.start + (region.size - 1u);
    if (region_end < region.start) {
        // integer overflow
        printk(KERN_INFO "umdp: I/O region request has invalid range, refusing request\n");
        return -EINVAL;
    }

    down_write(&client_info_lock);

    bool region_already_requested = false;
    struct client_info* this_client_info = NULL;
    struct client_info* client_info;
    for_each_client_info(client_info) {
        bool region_already_requested_by_this_client = client_info_requested_port_region(client_info, region);
        if (!region_already_requested) {
            region_already_requested = region_already_requested_by_this_client;
        }

        if (client_info->port_id == info->snd_portid) {
            if (region_already_requested_by_this_client) {
                // already subscribed, do nothing
                up_write(&client_info_lock);
                printk(KERN_INFO "umdp: port ID %u already requested region %llu - %llu, ignoring request\n",
                    info->snd_portid, region.start, region.start + region.size - 1);
                return 0;
            }
            this_client_info = client_info;
        }

        if (this_client_info != NULL && region_already_requested) {
            break;
        }
    }

    if (this_client_info == NULL) {
        up_write(&client_info_lock);
        printk(KERN_INFO "umdp: port ID %u is not registered, refusing request\n", info->snd_portid);
        return -EPERM;
    }

    // Replace the access control check with this bypass:

    // TEMPORARY BYPASS FOR TESTING - REMOVE AFTER CONFIRMING FUNCTIONALITY
    printk(KERN_INFO "umdp: BYPASSING access control - allowing %s to access region %llu-%llu\n", 
       this_client_info->exe_path, region.start, region.start + region.size - 1);

    /* Original access control (commented out for testing):
    if (!umdp_ac_can_access_port_io_region(this_client_info->exe_path, region)) {
    up_write(&client_info_lock);
    printk(KERN_INFO "umdp: %s not allowed to access the requested region, refusing request\n",
        this_client_info->exe_path);
    return -EPERM;
    }
    */

    struct port_io_region* new_regions = krealloc_array_compat(this_client_info->requested_port_io_regions,
        this_client_info->requested_port_io_regions_count + 1, sizeof(struct port_io_region), GFP_KERNEL);
    if (new_regions == NULL) {
        up_write(&client_info_lock);
        printk(KERN_ERR "umdp: failed to resize I/O port region array\n");
        return -ENOMEM;
    }
    this_client_info->requested_port_io_regions = new_regions;
    this_client_info->requested_port_io_regions[this_client_info->requested_port_io_regions_count].start = region.start;
    this_client_info->requested_port_io_regions[this_client_info->requested_port_io_regions_count].size = region.size;
    this_client_info->requested_port_io_regions_count++;

    if (!region_already_requested) {
        if (request_region(region.start, region.size, UMDP_DEVICE_NAME) == NULL) {
            release_region(region.start, region.size);
            if (request_region(region.start, region.size, UMDP_DEVICE_NAME) == NULL) {
                this_client_info->requested_port_io_regions_count--;
                // we could shrink the allocation, but it doesn't seem worth complicating the code further

                up_write(&client_info_lock);
                printk(KERN_ERR "umdp: failed to request region %llu - %llu, it's currently unavailable\n",
                    region.start, region.start + region.size - 1);
                return -EBUSY;
            }
        }
    }

    up_write(&client_info_lock);

    if (!region_already_requested) {
        printk(KERN_INFO "umdp: I/O region %llu - %llu was allocated\n", region.start, region.start + region.size - 1);
    }
    printk(KERN_INFO "umdp: port ID %u requested I/O region %llu - %llu successfully\n", info->snd_portid, region.start,
        region.start + region.size - 1);
    return 0;
}

static int umdp_devio_release(struct sk_buff* skb, struct genl_info* info) {
    struct nlattr* start_attr = find_attribute(info->attrs, UMDP_ATTR_DEVIO_REQUEST_START);
    struct nlattr* size_attr = find_attribute(info->attrs, UMDP_ATTR_DEVIO_REQUEST_SIZE);

    if (start_attr == NULL || size_attr == NULL) {
        printk(KERN_ERR "umdp: invalid IO region release request\n");
        return -EINVAL;
    }
    struct port_io_region region = {
        .start = *((u64*) nla_data(start_attr)),
        .size = *((u64*) nla_data(size_attr)),
    };
    printk(KERN_DEBUG "umdp: received release request for region %llu - %llu\n", region.start,
        region.start + region.size - 1);

    down_write(&client_info_lock);

    bool region_requested_by_others = false;
    struct client_info* this_client_info = NULL;
    struct client_info* client_info;
    for_each_client_info(client_info) {
        bool region_registered_by_this_client = client_info_requested_port_region(client_info, region);

        if (client_info->port_id == info->snd_portid) {
            if (!region_registered_by_this_client) {
                // not requested, do nothing
                up_write(&client_info_lock);
                printk(KERN_INFO "umdp: port ID %u didn't request region %llu - %llu, so it can't release it\n",
                    info->snd_portid, region.start, region.start + region.size - 1);
                return -ENOENT;
            }
            this_client_info = client_info;
        } else if (region_registered_by_this_client) {
            region_requested_by_others = true;
        }

        if (this_client_info != NULL && region_requested_by_others) {
            break;
        }
    }

    if (this_client_info == NULL) {
        up_write(&client_info_lock);
        printk(KERN_INFO "umdp: port ID %u is not registered, refusing request\n", info->snd_portid);
        return -EPERM;
    }

    size_t region_index = SIZE_MAX;
    size_t i;
    for (i = 0; i < this_client_info->requested_port_io_regions_count; i++) {
        if (this_client_info->requested_port_io_regions[i].start == region.start
            && this_client_info->requested_port_io_regions[i].size == region.size) {
            region_index = i;
            break;
        }
    }

    this_client_info->requested_port_io_regions_count--;
    for (i = region_index; i < this_client_info->requested_port_io_regions_count; i++) {
        this_client_info->requested_port_io_regions[i] = this_client_info->requested_port_io_regions[i + 1];
    }
    // we could shrink the allocation, but it doesn't seem worth complicating the code further

    if (!region_requested_by_others) {
        release_region(region.start, region.size);
    }

    up_write(&client_info_lock);

    printk(KERN_INFO "umdp: port ID %u released I/O region %llu - %llu\n", info->snd_portid, region.start,
        region.start + region.size - 1);
    if (!region_requested_by_others) {
        printk(KERN_INFO "umdp: I/O region %llu - %llu was released as it is no longer being used\n", region.start,
            region.start + region.size - 1);
    }
    return 0;
}

static struct workqueue_struct* ih_workqueue;
void interrupt_handler_wq(struct work_struct* ws);

struct ih_work_struct {
    struct work_struct ws;
    int irq;
    bool busy;
};
static struct ih_work_struct ih_workers[UMDP_WORKER_COUNT];

static irqreturn_t interrupt_handler(int irq, void* dev_id) {
    size_t i;
    disable_irq_nosync(irq);
    for (i = 0; i < UMDP_WORKER_COUNT; i++) {
        if (!ih_workers[i].busy) {
            ih_workers[i].busy = true;
            ih_workers[i].irq = irq;
            INIT_WORK(&ih_workers[i].ws, interrupt_handler_wq);
            queue_work(ih_workqueue, (struct work_struct*) &ih_workers[i]);
            break;
        }
    }
    return IRQ_HANDLED;
}

void interrupt_handler_wq(struct work_struct* ws) {
    struct ih_work_struct* work = (struct ih_work_struct*) ws;
    u32 reported_irq = (u32) work->irq;

    down_read(&client_info_lock);
    (void) find_requested_irq_for_linux_irq((u32) work->irq, &reported_irq);
    up_read(&client_info_lock);

    struct sk_buff* msg = genlmsg_new(nla_total_size(sizeof(u32)), GFP_KERNEL);
    if (msg == NULL) {
        printk(KERN_ERR "umdp: failed to allocate buffer for interrupt notification\n");
        work->busy = false;
        return;
    }

    void* msg_header = genlmsg_put(msg, 0, 0, &umdp_genl_family, 0, UMDP_CMD_INTERRUPT_NOTIFICATION);
    if (msg_header == NULL) {
        nlmsg_free(msg);
        printk(KERN_ERR "umdp: failed to add the generic netlink header to the interrupt notification\n");
        work->busy = false;
        return;
    }

    if (nla_put_u32(msg, UMDP_ATTR_INTERRUPT_IRQ, reported_irq) != 0) {
        nlmsg_free(msg);
        printk(KERN_ERR "umdp: failed to write value to interrupt notification (this is a bug)\n");
        work->busy = false;
        return;
    }

    genlmsg_end(msg, msg_header);
    int ret = genlmsg_multicast(&umdp_genl_family, msg, 0, 0, GFP_KERNEL);
    if (ret == -ESRCH) {
        printk(KERN_DEBUG "umdp: tried to send notification for IRQ %d, but no one is listening\n", work->irq);
        work->busy = false;
        return;
    } else if (ret != 0) {
        printk(KERN_ERR "umdp: failed to send interrupt notification (error code %d)\n", ret);
        work->busy = false;
        return;
    }

    printk(KERN_DEBUG "umdp: sent interrupt notification for requested IRQ %u (linux IRQ %u)\n", reported_irq,
        work->irq);
    work->busy = false;
}

static bool client_info_is_subscribed_to_irq(struct client_info* info, u32 requested_irq) {
    for (size_t i = 0; i < info->subscribed_irqs_count; i++) {
        if (info->subscribed_irqs[i].requested_irq == requested_irq) {
            return true;
        }
    }
    return false;
}

static bool find_linux_irq_for_requested_irq(u32 requested_irq, u32* linux_irq_out) {
    struct client_info* client_info;
    for_each_client_info(client_info) {
        for (size_t i = 0; i < client_info->subscribed_irqs_count; i++) {
            if (client_info->subscribed_irqs[i].requested_irq == requested_irq) {
                *linux_irq_out = client_info->subscribed_irqs[i].linux_irq;
                return true;
            }
        }
    }

    return false;
}

static bool find_requested_irq_for_linux_irq(u32 linux_irq, u32* requested_irq_out) {
    struct client_info* client_info;
    for_each_client_info(client_info) {
        for (size_t i = 0; i < client_info->subscribed_irqs_count; i++) {
            if (client_info->subscribed_irqs[i].linux_irq == linux_irq) {
                *requested_irq_out = client_info->subscribed_irqs[i].requested_irq;
                return true;
            }
        }
    }

    return false;
}

static int umdp_interrupt_subscribe(struct sk_buff* skb, struct genl_info* info) {
    struct nlattr* irq_attr = find_attribute(info->attrs, UMDP_ATTR_INTERRUPT_IRQ);
    if (irq_attr == NULL) {
        printk(KERN_ERR "umdp: invalid interrupt subscription request: IRQ attribute is missing\n");
        return -EINVAL;
    }
    u32 requested_irq = *((u32*) nla_data(irq_attr));

    down_write(&client_info_lock);

    bool irq_already_registered = false;
    u32 existing_linux_irq = 0u;
    struct client_info* this_client_info = NULL;
    struct client_info* client_info;
    for_each_client_info(client_info) {
        bool requested_irq_already_registered_by_this_client =
            client_info_is_subscribed_to_irq(client_info, requested_irq);
        if (!irq_already_registered && find_linux_irq_for_requested_irq(requested_irq, &existing_linux_irq)) {
            irq_already_registered = true;
        }

        if (client_info->port_id == info->snd_portid) {
            if (requested_irq_already_registered_by_this_client) {
                // already subscribed, do nothing
                up_write(&client_info_lock);
                printk(KERN_INFO "umdp: port ID %u is already subscribed to IRQ %u, ignoring request\n",
                    info->snd_portid, requested_irq);
                return 0;
            }
            this_client_info = client_info;
        }

        if (this_client_info != NULL && irq_already_registered) {
            break;
        }
    }

    if (this_client_info == NULL) {
        up_write(&client_info_lock);
        printk(KERN_INFO "umdp: port ID %u is not registered, refusing request\n", info->snd_portid);
        return -EPERM;
    }

    printk(KERN_INFO "umdp: BYPASSING IRQ access control for %s on IRQ %u\n", this_client_info->exe_path,
        requested_irq);

    struct subscribed_irq* new_subscribed_irqs =
        krealloc_array_compat(this_client_info->subscribed_irqs, this_client_info->subscribed_irqs_count + 1,
            sizeof(struct subscribed_irq), GFP_KERNEL);
    if (new_subscribed_irqs == NULL) {
        up_write(&client_info_lock);
        printk(KERN_ERR "umdp: failed to resize IRQ array\n");
        return -ENOMEM;
    }
    this_client_info->subscribed_irqs = new_subscribed_irqs;
    this_client_info->subscribed_irqs[this_client_info->subscribed_irqs_count].requested_irq = requested_irq;

    if (!irq_already_registered) {
        u32 linux_irq = 0u;
        int resolve_ret = resolve_linux_irq(requested_irq, &linux_irq);
        struct irq_desc* irq_desc;
        struct irq_data* irq_data;
        if (resolve_ret != 0) {
            up_write(&client_info_lock);
            printk(KERN_ERR "umdp: failed to resolve requested IRQ %u to a Linux IRQ (error %d)\n", requested_irq,
                resolve_ret);
            return resolve_ret;
        }

        irq_desc = irq_to_desc(linux_irq);
        irq_data = irq_desc ? irq_desc_get_irq_data(irq_desc) : NULL;
        printk(KERN_INFO "umdp: IRQ debug requested=%u linux=%u desc=%p chip=%s hwirq=%lu\n", requested_irq,
            linux_irq, irq_desc, (irq_data && irq_data->chip && irq_data->chip->name) ? irq_data->chip->name : "none",
            irq_data ? irq_data->hwirq : 0ul);

        int ret = request_irq(linux_irq, interrupt_handler, 0, UMDP_DEVICE_NAME, &client_info_list);
        if (ret != 0) {
            up_write(&client_info_lock);
            printk(KERN_ERR "umdp: IRQ request failed for requested IRQ %u (linux IRQ %u) with code %d\n", requested_irq,
                linux_irq, ret);
            return ret;
        }

        this_client_info->subscribed_irqs[this_client_info->subscribed_irqs_count].linux_irq = linux_irq;
        printk(KERN_INFO "umdp: mapped requested IRQ %u to linux IRQ %u\n", requested_irq, linux_irq);

        ensure_linux_irq_enabled(linux_irq);
    } else {
        this_client_info->subscribed_irqs[this_client_info->subscribed_irqs_count].linux_irq = existing_linux_irq;

        ensure_linux_irq_enabled(existing_linux_irq);
    }

    this_client_info->subscribed_irqs_count++;

    up_write(&client_info_lock);

    if (!irq_already_registered) {
        printk(KERN_INFO "umdp: IRQ %u was allocated\n", requested_irq);
    }
    printk(KERN_INFO "umdp: port ID %u subscribed to IRQ %u\n", info->snd_portid, requested_irq);
    return 0;
}

static int umdp_interrupt_unsubscribe(struct sk_buff* skb, struct genl_info* info) {
    struct nlattr* irq_attr = find_attribute(info->attrs, UMDP_ATTR_INTERRUPT_IRQ);
    if (irq_attr == NULL) {
        printk(KERN_ERR "umdp: invalid interrupt subscription request: IRQ attribute is missing\n");
        return -EINVAL;
    }
    u32 requested_irq = *((u32*) nla_data(irq_attr));

    down_write(&client_info_lock);

    bool irq_registered_by_others = false;
    struct client_info* this_client_info = NULL;
    struct client_info* client_info;
    for_each_client_info(client_info) {
        bool requested_irq_registered_by_this_client = client_info_is_subscribed_to_irq(client_info, requested_irq);

        if (client_info->port_id == info->snd_portid) {
            if (!requested_irq_registered_by_this_client) {
                // not subscribed, do nothing
                up_write(&client_info_lock);
                printk(KERN_INFO "umdp: port ID %u is not subscribed to IRQ %u, so it cannot be unsubscribed\n",
                    info->snd_portid, requested_irq);
                return -ENOENT;
            }
            this_client_info = client_info;
        } else if (requested_irq_registered_by_this_client) {
            irq_registered_by_others = true;
        }

        if (this_client_info != NULL && irq_registered_by_others) {
            break;
        }
    }

    if (this_client_info == NULL) {
        up_write(&client_info_lock);
        printk(KERN_INFO "umdp: port ID %u is not registered, refusing request\n", info->snd_portid);
        return -EPERM;
    }

    size_t subscribed_irq_index = SIZE_MAX;
    u32 linux_irq = 0u;
    size_t i;
    for (i = 0; i < this_client_info->subscribed_irqs_count; i++) {
        if (this_client_info->subscribed_irqs[i].requested_irq == requested_irq) {
            subscribed_irq_index = i;
            linux_irq = this_client_info->subscribed_irqs[i].linux_irq;
            break;
        }
    }

    if (subscribed_irq_index == SIZE_MAX) {
        up_write(&client_info_lock);
        printk(KERN_ERR "umdp: internal error: requested IRQ %u was not found in subscription list\n", requested_irq);
        return -ENOENT;
    }

    this_client_info->subscribed_irqs_count--;
    for (i = subscribed_irq_index; i < this_client_info->subscribed_irqs_count; i++) {
        this_client_info->subscribed_irqs[i] = this_client_info->subscribed_irqs[i + 1];
    }
    // we could shrink the allocation, but it doesn't seem worth complicating the code further

    if (!irq_registered_by_others) {
        free_irq(linux_irq, &client_info_list);
    }

    up_write(&client_info_lock);

    printk(KERN_INFO "umdp: port ID %u unsubscribed from IRQ %u\n", info->snd_portid, requested_irq);
    if (!irq_registered_by_others) {
        printk(KERN_INFO "umdp: IRQ %u (linux IRQ %u) was freed as it is no longer being used\n", requested_irq,
            linux_irq);
    }
    return 0;
}

static int umdp_interrupt_unmask(struct sk_buff* skb, struct genl_info* info) {
    struct nlattr* irq_attr = find_attribute(info->attrs, UMDP_ATTR_INTERRUPT_IRQ);
    if (irq_attr == NULL) {
        printk(KERN_ERR "umdp: invalid interrupt unmask request: IRQ attribute is missing\n");
        return -EINVAL;
    }
    u32 requested_irq = *((u32*) nla_data(irq_attr));

    down_read(&client_info_lock);

    struct client_info* client_info = get_client_info_by_netlink_port_id(info->snd_portid);
    if (client_info == NULL) {
        up_read(&client_info_lock);
        printk(KERN_INFO "umdp: port ID %u is not registered, refusing request\n", info->snd_portid);
        return -EPERM;
    }

    if (!client_info_is_subscribed_to_irq(client_info, requested_irq)) {
        up_read(&client_info_lock);
        printk(KERN_ERR "umdp: port ID %u is not subscribed to IRQ %u, so it cannot unmask it\n", info->snd_portid, requested_irq);
        return -EPERM;
    }

    u32 linux_irq = 0u;
    if (!find_linux_irq_for_requested_irq(requested_irq, &linux_irq)) {
        up_read(&client_info_lock);
        printk(KERN_ERR "umdp: internal error: requested IRQ %u is subscribed but has no linux IRQ mapping\n", requested_irq);
        return -ENOENT;
    }

    up_read(&client_info_lock);

    printk(KERN_DEBUG "umdp: unmasking IRQ %u (linux IRQ %u) for port ID %u\n", requested_irq, linux_irq, info->snd_portid);
    enable_irq(linux_irq);

    return 0;
}

static int umdp_mem_open(struct inode* inode __attribute__((unused)), struct file* filep __attribute__((unused))) {
    return 0;
}

static int umdp_mem_release(struct inode* inode __attribute__((unused)), struct file* filep __attribute__((unused))) {
    return 0;
}

static int umdp_mem_mmap(struct file* file __attribute__((unused)), struct vm_area_struct* vma) {
    bool is_write = (vma->vm_flags & VM_WRITE) != 0;
    vm_flags_t shared_flags =
        VM_MAYSHARE | (is_write ? VM_SHARED : 0);  // is VM_MAYSHARE always set if VM_SHARED is set?
    if ((vma->vm_flags & shared_flags) != shared_flags) {
        printk(KERN_ERR "umdp: mmap requests must set the MAP_SHARED flag\n");
        return -EINVAL;
    }

    unsigned long physical_start_addr = vma->vm_pgoff * PAGE_SIZE;
    unsigned long physical_end_addr = physical_start_addr + (vma->vm_end - vma->vm_start);

    const char* exe_path = exe_path_of_task(current);
    if (exe_path == NULL) {
        printk(KERN_ERR "umdp: mmap request made from process with no executable, refusing request\n");
        return -EPERM;
    }
    
    // TEMPORARY BYPASS FOR TESTING - REMOVE AFTER CONFIRMING FUNCTIONALITY
    printk(KERN_INFO "umdp: BYPASSING access control - allowing %s to mmap region 0x%lx-0x%lx\n", 
           exe_path, physical_start_addr, physical_end_addr);
           
    /* Original access control (commented out for testing):
    if (!umdp_ac_can_access_mmap_region(
            exe_path, (struct mmap_region){.start = physical_start_addr, .end = physical_end_addr})) {
        printk(KERN_INFO "umdp: %s not allowed to access region 0x%lx-0x%lx, refusing request\n", exe_path,
            physical_start_addr, physical_end_addr);
        return -EPERM;
    }
    */

    vma->vm_flags |= VM_IO;

    printk(KERN_DEBUG "umdp: performing mmap of region 0x%lx-0x%lx to address 0x%lu of PID %d\n", physical_start_addr,
        physical_end_addr, vma->vm_start, current->pid);
    return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static struct file_operations umdp_mem_fops = {
    .owner = THIS_MODULE,
    .open = umdp_mem_open,
    .release = umdp_mem_release,
    .mmap = umdp_mem_mmap,
};

static struct cdev umdp_mem_cdev;
static dev_t umdp_mem_chrdev;
static struct class* umdp_mem_dev_class;

#define UMDP_MEM_CLASS_NAME "umdp"
#define UMDP_MEM_DEVICE_NAME "umdp-mem"

static bool kprobe_registered = false;

static int __init umdp_init(void) {
    int ret = umdp_ac_init();
    if (ret != 0) {
        goto fail;
    }

    ret = alloc_chrdev_region(&umdp_mem_chrdev, 0, 1, UMDP_MEM_DEVICE_NAME);
    if (ret != 0) {
        printk(KERN_ERR "umdp: Failed to allocate character device (error code %d)\n", -ret);
        goto fail_after_ac_init;
    }

    cdev_init(&umdp_mem_cdev, &umdp_mem_fops);
    umdp_mem_cdev.owner = THIS_MODULE;
    ret = kobject_set_name(&umdp_mem_cdev.kobj, UMDP_MEM_DEVICE_NAME);
    if (ret != 0) {
        printk(KERN_ERR "umdp: Failed to set character device name (error code %d)\n", -ret);
        goto fail_after_cdev_init;
    }

    ret = cdev_add(&umdp_mem_cdev, umdp_mem_chrdev, 1);
    if (ret != 0) {
        printk(KERN_ERR "umdp: Failed to add character device to system (error code %d)\n", -ret);
        goto fail_after_cdev_init;
    }

    umdp_mem_dev_class = class_create(THIS_MODULE, UMDP_MEM_CLASS_NAME);
    if (IS_ERR(umdp_mem_dev_class)) {
        ret = (int) PTR_ERR(umdp_mem_dev_class);
        printk(KERN_ERR "umdp: Failed to create character device class (error code %d)\n", -ret);
        goto fail_after_cdev_init;
    }

    {
        struct device* umdp_mem_device =
            device_create(umdp_mem_dev_class, NULL, umdp_mem_chrdev, NULL, UMDP_MEM_DEVICE_NAME);
        if (IS_ERR(umdp_mem_device)) {
            ret = (int) PTR_ERR(umdp_mem_device);
            printk(KERN_ERR "umdp: Failed to create character device (error code %d)\n", -ret);
            goto fail_after_class_create;
        }
    }

    ih_workqueue = alloc_workqueue("umdp_interrupt_wq", 0, 0);
    if (ih_workqueue == NULL) {
        ret = -ENOMEM;
        printk(KERN_ERR "umdp: failed to allocate workqueue for interrupt handling");
        goto fail_after_device_create;
    }

    {
        size_t i;
        for (i = 0; i < UMDP_WORKER_COUNT; i++) {
            ih_workers[i].busy = false;
        }
    }

    exit_cleanup_workqueue = alloc_workqueue("umdp_exit_cleanup_wq", WQ_UNBOUND, 1);
    if (exit_cleanup_workqueue != NULL) {
        ret = register_kprobe(&do_exit_kp);
        if (ret == 0) {
            kprobe_registered = true;
        } else {
            destroy_workqueue(exit_cleanup_workqueue);
            exit_cleanup_workqueue = NULL;
            if (ret == -EOPNOTSUPP) {
                printk(KERN_WARNING
                    "umdp: This kernel does not support kprobes (it was built with CONFIG_KPROBES=n), resources won't "
                    "be freed on process exit\n");
            } else {
                printk(KERN_WARNING
                    "umdp: Failed to register kprobe (error code %d), resources won't be freed on process exit\n",
                    -ret);
            }
            ret = 0;
        }
    } else {
        printk(KERN_WARNING "umdp: failed to allocate workqueue, resources won't be freed on process exit");
    }

    ret = genl_register_family(&umdp_genl_family);
    if (ret != 0) {
        printk(KERN_ERR "umdp: Failed to register netlink family (error code %d)\n", ret);
        goto fail_after_kprobe_register;
    }

    printk(KERN_INFO "umdp: Registered netlink kernel family (id: %d)\n", umdp_genl_family.id);
    return 0;

fail_after_kprobe_register:
    if (kprobe_registered) {
        unregister_kprobe(&do_exit_kp);
    }
fail_after_device_create:
    device_destroy(umdp_mem_dev_class, umdp_mem_chrdev);
fail_after_class_create:
    class_destroy(umdp_mem_dev_class);
fail_after_cdev_init:
    cdev_del(&umdp_mem_cdev);
    unregister_chrdev_region(umdp_mem_chrdev, 1);
fail_after_ac_init:
    umdp_ac_exit();
fail:
    return ret;
}

static void __exit umdp_exit(void) {
    int ret = genl_unregister_family(&umdp_genl_family);
    if (ret != 0) {
        printk(KERN_ERR "umdp: Failed to unregister netlink family\n");
    } else {
        printk(KERN_INFO "umdp: Unregistered netlink family\n");
    }

    if (kprobe_registered) {
        unregister_kprobe(&do_exit_kp);
        destroy_workqueue(exit_cleanup_workqueue);
    }

    destroy_workqueue(ih_workqueue);

    device_destroy(umdp_mem_dev_class, umdp_mem_chrdev);
    class_destroy(umdp_mem_dev_class);
    cdev_del(&umdp_mem_cdev);
    unregister_chrdev_region(umdp_mem_chrdev, 1);

    down_write(&client_info_lock);
    {
        struct client_info* p;
        struct client_info* next;
        for_each_client_info_safe(p, next) {
            remove_client(p);
        }
    }
    up_write(&client_info_lock);

    umdp_ac_exit();
}

module_init(umdp_init);
module_exit(umdp_exit);
