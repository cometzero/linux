// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/uaccess.h>

#define CONTROL_SIZE	32
#define TIMER_DELAY_NS	(2 * NSEC_PER_MSEC)

static unsigned int target_cpu = 1;
module_param(target_cpu, uint, 0444);
MODULE_PARM_DESC(target_cpu, "Initial CPU targeted by the test SPI");

enum gic720ae_operation {
	GIC720AE_IPI,
	GIC720AE_SPI,
	GIC720AE_SNAPSHOT,
};

struct gic720ae_command {
	enum gic720ae_operation operation;
	unsigned int cpu;
};

struct gic720ae_cpu_state {
	struct hrtimer timer;
	struct completion ppi_complete;
	atomic_t ipi_count;
	atomic_t ppi_count;
	atomic_t spi_count;
	int ppi_before;
};

struct gic720ae_test {
	struct device *dev;
	struct gic720ae_cpu_state __percpu *cpu_state;
	struct dentry *debugfs_dir;
	struct completion spi_complete;
	struct mutex control_lock;
	atomic_t spi_armed;
	unsigned int spi_irq;
	unsigned int spi_observed_cpu;
};

static DEFINE_MUTEX(active_lock);
static struct gic720ae_test *active_test;

static enum hrtimer_restart gic720ae_timer_fn(struct hrtimer *timer)
{
	struct gic720ae_cpu_state *state =
		container_of(timer, struct gic720ae_cpu_state, timer);

	atomic_inc(&state->ppi_count);
	complete(&state->ppi_complete);
	return HRTIMER_NORESTART;
}

static void gic720ae_ipi_fn(void *arg)
{
	struct gic720ae_test *test = arg;
	unsigned int cpu = smp_processor_id();

	atomic_inc(&per_cpu_ptr(test->cpu_state, cpu)->ipi_count);
}

static void gic720ae_start_timer(void *arg)
{
	struct gic720ae_test *test = arg;
	struct gic720ae_cpu_state *state;

	state = this_cpu_ptr(test->cpu_state);
	hrtimer_start(&state->timer, ns_to_ktime(TIMER_DELAY_NS),
		      HRTIMER_MODE_REL_PINNED_HARD);
}

static irqreturn_t gic720ae_spi_handler(int irq, void *arg)
{
	struct gic720ae_test *test = arg;
	unsigned int cpu = raw_smp_processor_id();

	atomic_inc(&per_cpu_ptr(test->cpu_state, cpu)->spi_count);
	WRITE_ONCE(test->spi_observed_cpu, cpu);
	if (atomic_cmpxchg(&test->spi_armed, 1, 0) == 1)
		complete(&test->spi_complete);

	return IRQ_HANDLED;
}

static int gic720ae_validate_cpu(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return -ERANGE;
	if (!cpu_online(cpu))
		return -ENODEV;

	return 0;
}

static int gic720ae_validate_affinity(struct gic720ae_test *test,
				      unsigned int cpu)
{
	const struct cpumask *effective;

	effective = irq_get_effective_affinity_mask(test->spi_irq);
	if (!effective || !cpumask_test_cpu(cpu, effective))
		return -EXDEV;

	return 0;
}

static int gic720ae_set_affinity(struct gic720ae_test *test, unsigned int cpu)
{
	int ret;

	ret = irq_set_affinity(test->spi_irq, cpumask_of(cpu));
	if (ret)
		return ret;

	return gic720ae_validate_affinity(test, cpu);
}

static int gic720ae_run_ipi(struct gic720ae_test *test, unsigned int cpu)
{
	int count;
	int ret;

	cpus_read_lock();
	ret = gic720ae_validate_cpu(cpu);
	if (!ret)
		ret = smp_call_function_single(cpu, gic720ae_ipi_fn, test, 1);
	cpus_read_unlock();
	if (ret)
		return ret;

	count = atomic_read(&per_cpu_ptr(test->cpu_state, cpu)->ipi_count);
	dev_info(test->dev, "ipi target=%u count=%d\n", cpu, count);
	return 0;
}

static int gic720ae_run_spi(struct gic720ae_test *test, unsigned int cpu)
{
	int count;
	int ret;

	cpus_read_lock();
	ret = gic720ae_validate_cpu(cpu);
	if (ret)
		goto unlock;

	ret = gic720ae_set_affinity(test, cpu);
	if (ret)
		goto unlock;

	reinit_completion(&test->spi_complete);
	WRITE_ONCE(test->spi_observed_cpu, nr_cpu_ids);
	atomic_set(&test->spi_armed, 1);
	ret = irq_set_irqchip_state(test->spi_irq, IRQCHIP_STATE_PENDING, true);
	if (ret) {
		atomic_set(&test->spi_armed, 0);
		goto unlock;
	}
	if (!wait_for_completion_timeout(&test->spi_complete, HZ)) {
		atomic_set(&test->spi_armed, 0);
		synchronize_irq(test->spi_irq);
		irq_set_irqchip_state(test->spi_irq, IRQCHIP_STATE_PENDING, false);
		ret = -ETIMEDOUT;
		goto unlock;
	}
	if (READ_ONCE(test->spi_observed_cpu) != cpu) {
		ret = -EXDEV;
		goto unlock;
	}

	count = atomic_read(&per_cpu_ptr(test->cpu_state, cpu)->spi_count);
	dev_info(test->dev, "spi target=%u cpu=%u count=%d\n", cpu,
		 READ_ONCE(test->spi_observed_cpu), count);
unlock:
	cpus_read_unlock();
	return ret;
}

static int gic720ae_snapshot(struct gic720ae_test *test)
{
	unsigned int cpu;
	int ret = 0;

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct gic720ae_cpu_state *state;

		state = per_cpu_ptr(test->cpu_state, cpu);
		hrtimer_cancel(&state->timer);
		reinit_completion(&state->ppi_complete);
		state->ppi_before = atomic_read(&state->ppi_count);
		ret = smp_call_function_single(cpu, gic720ae_start_timer, test, 1);
		if (ret)
			goto cancel_timers;
	}

	for_each_online_cpu(cpu) {
		struct gic720ae_cpu_state *state;
		int delta;

		state = per_cpu_ptr(test->cpu_state, cpu);
		if (!wait_for_completion_timeout(&state->ppi_complete, HZ)) {
			ret = -ETIMEDOUT;
			goto cancel_timers;
		}
		delta = atomic_read(&state->ppi_count) - state->ppi_before;
		if (delta != 1) {
			ret = -EIO;
			goto cancel_timers;
		}
		dev_info(test->dev,
			 "snapshot cpu=%u ipi=%d arch_timer_ppi_delta=%d spi=%d\n",
			 cpu, atomic_read(&state->ipi_count), delta,
			 atomic_read(&state->spi_count));
	}
	goto unlock;

cancel_timers:
	for_each_online_cpu(cpu)
		hrtimer_cancel(&per_cpu_ptr(test->cpu_state, cpu)->timer);
unlock:
	cpus_read_unlock();
	return ret;
}

static int gic720ae_parse_command(char *command,
				  struct gic720ae_command *parsed)
{
	char *argument;
	char *cursor;
	size_t len;

	len = strlen(command);
	if (!len)
		return -EINVAL;
	if (command[len - 1] == '\n')
		command[--len] = '\0';
	if (!len || strchr(command, '\n') || strchr(command, '\r'))
		return -EINVAL;

	if (sysfs_streq(command, "snapshot")) {
		parsed->operation = GIC720AE_SNAPSHOT;
		return 0;
	}
	if (!strncmp(command, "ipi ", 4))
		parsed->operation = GIC720AE_IPI;
	else if (!strncmp(command, "spi ", 4))
		parsed->operation = GIC720AE_SPI;
	else
		return -EINVAL;

	argument = command + 4;
	if (!*argument)
		return -EINVAL;
	for (cursor = argument; *cursor; cursor++) {
		if (!isdigit(*cursor))
			return -EINVAL;
	}

	return kstrtouint(argument, 10, &parsed->cpu);
}

static ssize_t gic720ae_control_write(struct file *file,
				      const char __user *user_buf,
				      size_t count, loff_t *ppos)
{
	struct gic720ae_test *test = file->private_data;
	struct gic720ae_command parsed;
	char command[CONTROL_SIZE];
	int ret;

	if (!count || count >= sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, user_buf, count))
		return -EFAULT;
	command[count] = '\0';
	if (strnlen(command, count) != count)
		return -EINVAL;

	ret = gic720ae_parse_command(command, &parsed);
	if (ret)
		return ret;

	mutex_lock(&test->control_lock);
	switch (parsed.operation) {
	case GIC720AE_IPI:
		ret = gic720ae_run_ipi(test, parsed.cpu);
		break;
	case GIC720AE_SPI:
		ret = gic720ae_run_spi(test, parsed.cpu);
		break;
	case GIC720AE_SNAPSHOT:
		ret = gic720ae_snapshot(test);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&test->control_lock);

	return ret ? ret : count;
}

static const struct file_operations gic720ae_control_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gic720ae_control_write,
	.llseek = noop_llseek,
};

static int gic720ae_probe(struct platform_device *pdev)
{
	struct gic720ae_test *test;
	struct dentry *control;
	struct irq_data *irq_data;
	unsigned int cpu;
	int irq_count;
	int irq;
	int ret;

	mutex_lock(&active_lock);
	if (active_test) {
		ret = -EBUSY;
		goto unlock_active;
	}
	if (!pdev->dev.of_node) {
		ret = -ENODEV;
		goto unlock_active;
	}

	cpus_read_lock();
	ret = gic720ae_validate_cpu(target_cpu);
	cpus_read_unlock();
	if (ret)
		goto unlock_active;

	irq_count = platform_irq_count(pdev);
	if (irq_count < 0) {
		ret = irq_count;
		goto unlock_active;
	}
	if (irq_count != 1) {
		ret = -EINVAL;
		goto unlock_active;
	}
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto unlock_active;
	}
	irq_data = irq_get_irq_data(irq);
	if (!irq_data || irqd_is_per_cpu(irq_data) ||
	    irqd_to_hwirq(irq_data) < 32) {
		ret = -EINVAL;
		goto unlock_active;
	}

	test = devm_kzalloc(&pdev->dev, sizeof(*test), GFP_KERNEL);
	if (!test) {
		ret = -ENOMEM;
		goto unlock_active;
	}
	test->cpu_state = alloc_percpu(struct gic720ae_cpu_state);
	if (!test->cpu_state) {
		ret = -ENOMEM;
		goto unlock_active;
	}
	test->dev = &pdev->dev;
	test->spi_irq = irq;
	test->spi_observed_cpu = nr_cpu_ids;
	init_completion(&test->spi_complete);
	mutex_init(&test->control_lock);
	atomic_set(&test->spi_armed, 0);
	for_each_possible_cpu(cpu) {
		struct gic720ae_cpu_state *state;

		state = per_cpu_ptr(test->cpu_state, cpu);
		hrtimer_setup(&state->timer, gic720ae_timer_fn, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_PINNED_HARD);
		init_completion(&state->ppi_complete);
		atomic_set(&state->ipi_count, 0);
		atomic_set(&state->ppi_count, 0);
		atomic_set(&state->spi_count, 0);
	}

	ret = request_irq(test->spi_irq, gic720ae_spi_handler, 0,
			  dev_name(&pdev->dev), test);
	if (ret)
		goto free_percpu;
	cpus_read_lock();
	ret = gic720ae_set_affinity(test, target_cpu);
	cpus_read_unlock();
	if (ret)
		goto free_irq;

	test->debugfs_dir = debugfs_create_dir("gic720ae_test", NULL);
	if (IS_ERR_OR_NULL(test->debugfs_dir)) {
		ret = test->debugfs_dir ? PTR_ERR(test->debugfs_dir) : -ENOMEM;
		goto free_irq;
	}
	control = debugfs_create_file("control", 0600, test->debugfs_dir, test,
				      &gic720ae_control_fops);
	if (IS_ERR_OR_NULL(control)) {
		ret = control ? PTR_ERR(control) : -ENOMEM;
		goto remove_debugfs;
	}

	platform_set_drvdata(pdev, test);
	active_test = test;
	mutex_unlock(&active_lock);
	dev_info(&pdev->dev, "test profile ready on SPI IRQ %u target CPU %u\n",
		 test->spi_irq, target_cpu);
	return 0;

remove_debugfs:
	debugfs_remove_recursive(test->debugfs_dir);
free_irq:
	free_irq(test->spi_irq, test);
free_percpu:
	free_percpu(test->cpu_state);
unlock_active:
	mutex_unlock(&active_lock);
	return ret;
}

static void gic720ae_remove(struct platform_device *pdev)
{
	struct gic720ae_test *test = platform_get_drvdata(pdev);
	unsigned int cpu;

	mutex_lock(&active_lock);
	debugfs_remove_recursive(test->debugfs_dir);
	mutex_lock(&test->control_lock);
	atomic_set(&test->spi_armed, 0);
	irq_set_irqchip_state(test->spi_irq, IRQCHIP_STATE_PENDING, false);
	for_each_possible_cpu(cpu)
		hrtimer_cancel(&per_cpu_ptr(test->cpu_state, cpu)->timer);
	free_irq(test->spi_irq, test);
	free_percpu(test->cpu_state);
	active_test = NULL;
	mutex_unlock(&test->control_lock);
	mutex_unlock(&active_lock);
}

static const struct of_device_id gic720ae_of_match[] = {
	{ .compatible = "arm,gic720ae-linux-selftest" },
	{ }
};
MODULE_DEVICE_TABLE(of, gic720ae_of_match);

static struct platform_driver gic720ae_driver = {
	.probe = gic720ae_probe,
	.remove = gic720ae_remove,
	.driver = {
		.name = "gic720ae-selftest",
		.of_match_table = gic720ae_of_match,
	},
};
module_platform_driver(gic720ae_driver);

MODULE_DESCRIPTION("Opt-in GIC-720AE Linux interrupt-path exerciser");
MODULE_LICENSE("GPL");
