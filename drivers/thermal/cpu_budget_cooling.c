/*
 *  linux/drivers/thermal/cpu_budget_cooling.c
 *
 *  Copyright (C) 2012	Samsung Electronics Co., Ltd(http://www.samsung.com)
 *  Copyright (C) 2012  Amit Daniel <amit.kachhap@linaro.org>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/platform_device.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpu_budget_cooling.h>
#include "thermal_core.h"

#define CREATE_TRACE_POINTS
#include <trace/events/budget_cooling.h>
#define BOOT_CPU    0

/**
 * struct cpu_budget_cooling_device
 * @id: unique integer value corresponding to each cpu_budget_cooling_device
 *	registered.
 * @cool_dev: thermal_cooling_device pointer to keep track of the the
 *	egistered cooling device.
 * @cpu_budget_state: integer value representing the current state of cpufreq
 *	cooling	devices.
 * @cpufreq_val: integer value representing the absolute value of the clipped
 *	frequency.
 * @allowed_cpus: all the cpus involved for this cpu_budget_cooling_device.
 * @node: list_head to link all cpu_budget_cooling_device together.
 *
 * This structure is required for keeping information of each
 * cpu_budget_cooling_device registered as a list whose head is represented by
 * cooling_cpufreq_list. In order to prevent corruption of this list a
 * mutex lock cooling_cpu_budget_lock is used.
 */

static LIST_HEAD(cooling_cpufreq_list);
static DEFINE_IDR(cpu_budget_idr);
static DEFINE_MUTEX(cooling_cpu_budget_lock);
static unsigned int cpu_budget_dev_count;
static struct cpu_budget_cooling_device* notify_device=NULL;
/**
 * get_idr - function to get a unique id.
 * @idr: struct idr * handle used to create a id.
 * @id: int * value generated by this function.
 */
static int get_idr(struct idr *idr, int *id)
{
	int err;
again:
	if (unlikely(idr_pre_get(idr, GFP_KERNEL) == 0))
		return -ENOMEM;

	mutex_lock(&cooling_cpu_budget_lock);
	err = idr_get_new(idr, NULL, id);
	mutex_unlock(&cooling_cpu_budget_lock);

	if (unlikely(err == -EAGAIN))
		goto again;
	else if (unlikely(err))
		return err;

	*id = *id & MAX_ID_MASK;
	return 0;
}

/**
 * release_idr - function to free the unique id.
 * @idr: struct idr * handle used for creating the id.
 * @id: int value representing the unique id.
 */
static void release_idr(struct idr *idr, int id)
{
	mutex_lock(&cooling_cpu_budget_lock);
	idr_remove(idr, id);
	mutex_unlock(&cooling_cpu_budget_lock);
}

/* Below code defines functions to be used for cpufreq as cooling device */

/**
 * is_cpufreq_valid - function to check if a cpu has frequency transition policy.
 * @cpu: cpu for which check is needed.
 */
static int is_cpufreq_valid(int cpu)
{
	struct cpufreq_policy policy;
	return !cpufreq_get_policy(&policy, cpu);
}
#ifdef CONFIG_HOTPLUG_CPU
static int get_any_online_cpu(const cpumask_t *mask)
{
	int cpu,lastcpu=0xffff;

	for_each_cpu(cpu, mask) {
		if ((cpu != BOOT_CPU) && cpu_online(cpu))
        {
            if(lastcpu == 0xffff)
                lastcpu = cpu;
            else if(cpu >lastcpu)
                lastcpu = cpu;
        }
	}
	return lastcpu;
}
static int get_online_cpu(const cpumask_t *mask)
{
	int cpu,num =0;

	for_each_cpu(cpu, mask) {
		if (cpu_online(cpu))
			num++;
	}
	return num;
}
#endif
static BLOCKING_NOTIFIER_HEAD(budget_cooling_notifier_list);
int register_budget_cooling_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&budget_cooling_notifier_list, nb);
}
EXPORT_SYMBOL(register_budget_cooling_notifier);

/**
 * cpu_budget_apply_cooling - function to apply frequency clipping.
 * @cpu_budget_device: cpu_budget_cooling_device pointer containing frequency
 *	clipping data.
 * @cooling_state: value of the cooling state.
 */
#ifdef CONFIG_CPU_FREQ_GOV_AUTO_HOTPLUG_ROOMAGE
extern int autohotplug_update_room(unsigned int c0min,unsigned int c1min,unsigned int c0max,unsigned int c1max);
#endif
int cpu_budget_update_state(struct cpu_budget_cooling_device *cpu_budget_device)
{
	int ret = 0;
	unsigned int cpuid;
#ifdef CONFIG_HOTPLUG_CPU
	int i = 0;
	unsigned int c0_online=0,c1_online=0;
	unsigned int c0_takedown=0,c1_takedown=0;
	unsigned int c0_max,c1_max,c0_min,c1_min;
#endif
	struct cpumask *cluster0_cpus = &cpu_budget_device->cluster0_cpus;
	struct cpumask *cluster1_cpus = &cpu_budget_device->cluster1_cpus;
	struct cpufreq_policy policy;

    ret = 0;
#ifdef CONFIG_HOTPLUG_CPU
// update cpu limit
    for_each_online_cpu(i) {
        if (cpumask_test_cpu(i, &cpu_budget_device->cluster0_cpus))
            c0_online++;
        else if (cpumask_test_cpu(i, &cpu_budget_device->cluster1_cpus))
            c1_online++;
    }

    c1_max = (cpu_budget_device->cluster1_num_roof >=cpu_budget_device->cluster1_num_limit)?
             cpu_budget_device->cluster1_num_limit:cpu_budget_device->cluster1_num_roof;
    c0_max = (cpu_budget_device->cluster0_num_roof >=cpu_budget_device->cluster0_num_limit)?
             cpu_budget_device->cluster0_num_limit:cpu_budget_device->cluster0_num_roof;
    c1_min = (cpu_budget_device->cluster1_num_floor >=c1_max)?
             c1_max:cpu_budget_device->cluster1_num_floor;
    c0_min = (cpu_budget_device->cluster0_num_floor >=c0_max)?
             c0_max:cpu_budget_device->cluster0_num_floor;
    c0_takedown = (c0_online > c0_max)?(c0_online - c0_max):0;
    c1_takedown = (c1_online > c1_max)?(c1_online - c1_max):0;
    while(c1_takedown)
    {
		cpuid = get_any_online_cpu(&cpu_budget_device->cluster1_cpus);
		if (cpuid < nr_cpu_ids)
        {
                    pr_info("CPU Budget:Try to down cpu %d, cluster1 online %d, max %d\n",cpuid,c1_online,c1_max);
			ret = work_on_cpu(BOOT_CPU,
					  (long(*)(void *))cpu_down,
					  (void *)cpuid);
        }
        c1_takedown--;
    }
    while(c0_takedown)
    {
		cpuid = get_any_online_cpu(&cpu_budget_device->cluster0_cpus);
		if (cpuid < nr_cpu_ids)
        {
                    pr_info("CPU Budget:Try to down cpu %d, cluster0 online %d, limit %d\n",cpuid,c0_online,cpu_budget_device->cluster0_num_limit);
			ret = work_on_cpu(BOOT_CPU,
					  (long(*)(void *))cpu_down,
					  (void *)cpuid);
        }
        c0_takedown--;
    }
#endif
#ifdef CONFIG_CPU_FREQ_GOV_AUTO_HOTPLUG_ROOMAGE
    autohotplug_update_room(c0_min,c1_min,c0_max,c1_max);
#endif

    // update gpu limit
    if(cpu_budget_device->gpu_throttle)
        blocking_notifier_call_chain(&budget_cooling_notifier_list,BUDGET_GPU_THROTTLE, NULL);
    else
        blocking_notifier_call_chain(&budget_cooling_notifier_list,BUDGET_GPU_UNTHROTTLE, NULL);

    // update cpufreq limit
	for_each_cpu(cpuid, cluster0_cpus) {
		if (is_cpufreq_valid(cpuid))
        {
            if((cpufreq_get_policy(&policy, cpuid) == 0) && policy.user_policy.governor)
            {
                cpufreq_update_policy(cpuid);
                break;
            }
        }
	}
	for_each_cpu(cpuid, cluster1_cpus) {
		if (is_cpufreq_valid(cpuid))
        {
            if((cpufreq_get_policy(&policy, cpuid) == 0) && policy.user_policy.governor)
            {
                cpufreq_update_policy(cpuid);
                break;
            }
        }
	}

	return ret;
}
EXPORT_SYMBOL(cpu_budget_update_state);

/**
 * cpu_budget_apply_cooling - function to apply frequency clipping.
 * @cpu_budget_device: cpu_budget_cooling_device pointer containing frequency
 *	clipping data.
 * @cooling_state: value of the cooling state.
 */
static int cpu_budget_apply_cooling(struct cpu_budget_cooling_device *cpu_budget_device,
				unsigned long cooling_state)
{
    unsigned long flags;
    struct thermal_instance *instance;
    int temperature = 0;

	/* Check if the old cooling action is same as new cooling action */
	if (cpu_budget_device->cpu_budget_state == cooling_state)
		return 0;

	cpu_budget_device->cpu_budget_state = cooling_state;

    if(cooling_state >= cpu_budget_device->tbl_num)
		return -EINVAL;

    spin_lock_irqsave(&cpu_budget_device->lock, flags);
	cpu_budget_device->cluster0_freq_limit = cpu_budget_device->tbl[cooling_state].cluster0_freq;
	cpu_budget_device->cluster0_num_limit = cpu_budget_device->tbl[cooling_state].cluster0_cpunr;
	cpu_budget_device->cluster1_freq_limit = cpu_budget_device->tbl[cooling_state].cluster1_freq;
	cpu_budget_device->cluster1_num_limit = cpu_budget_device->tbl[cooling_state].cluster1_cpunr;
	cpu_budget_device->gpu_throttle = cpu_budget_device->tbl[cooling_state].gpu_throttle;
    spin_unlock_irqrestore(&cpu_budget_device->lock, flags);

    trace_cpu_budget_throttle(cooling_state,
                              cpu_budget_device->cluster0_freq_limit,
                              cpu_budget_device->cluster0_num_limit ,
                              cpu_budget_device->cluster1_freq_limit,
                              cpu_budget_device->cluster1_num_limit,
                              cpu_budget_device->gpu_throttle);
	list_for_each_entry(instance, &(cpu_budget_device->cool_dev->thermal_instances), cdev_node) {
        if(instance->tz->temperature > temperature)
            temperature = instance->tz->temperature;
    }
/*    pr_info("CPU Budget: Temperature: %u Limit state:%lu item[%d,%d,%d,%d %d]\n",temperature,cooling_state,
    cpu_budget_device->cluster0_freq_limit,
    cpu_budget_device->cluster0_num_limit ,
    cpu_budget_device->cluster1_freq_limit ,
    cpu_budget_device->cluster1_num_limit,
    cpu_budget_device->gpu_throttle); */

    return cpu_budget_update_state(cpu_budget_device);
}
#ifdef CONFIG_HOTPLUG_CPU
static int hotplug_thermal_notifier(struct notifier_block *nfb,
					unsigned long action, void *hcpu)
{


    unsigned int c0_online=0,c1_online=0;
    unsigned int c0_max=0,c1_max=0;
//    unsigned int c0_min=0,c1_min=0;
	unsigned int cpu = (unsigned long)hcpu;

	switch (action) {
	case CPU_UP_PREPARE:
        c0_online = get_online_cpu(&notify_device->cluster0_cpus);
        c1_online = get_online_cpu(&notify_device->cluster1_cpus);
        if (cpumask_test_cpu(cpu, &notify_device->cluster1_cpus))
        {
            if(notify_device->cluster1_num_roof >=notify_device->cluster1_num_limit)
                c1_max = notify_device->cluster1_num_limit;
            else
                c1_max = notify_device->cluster1_num_roof;

            if(c1_online >= c1_max)
            {
                    pr_info("CPU Budget:reject cpu %d to up, cluster1 online %d, limit %d\n",cpu,c1_online,c1_max);
                   return NOTIFY_BAD;
            }
        }
         else if (cpumask_test_cpu(cpu, &notify_device->cluster0_cpus))
        {
            if(notify_device->cluster1_num_roof >=notify_device->cluster1_num_limit)
                c0_max = notify_device->cluster0_num_limit;
            else
                c0_max = notify_device->cluster0_num_roof;

            if(c0_online >= c0_max)
            {
                    pr_info("CPU Budget:reject cpu %d to up, cluster0 online %d, max %d\n",cpu,c0_online,c0_max);
                   return NOTIFY_BAD;
            }
        }
        return NOTIFY_DONE;
	default:
		break;
	}

	return NOTIFY_DONE;
}
#endif
/**
 * cpufreq_thermal_notifier - notifier callback for cpufreq policy change.
 * @nb:	struct notifier_block * with callback info.
 * @event: value showing cpufreq event for which this function invoked.
 * @data: callback-specific data
 */
static int cpufreq_thermal_notifier(struct notifier_block *nb,
					unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned long limit_freq=0,base_freq=0,head_freq=0;
    unsigned long max_freq=0,min_freq=0;

	if (event != CPUFREQ_ADJUST || notify_device == NOTIFY_INVALID)
		return 0;

	if (cpumask_test_cpu(policy->cpu, &notify_device->cluster0_cpus))
    {
                limit_freq = notify_device->cluster0_freq_limit;
                base_freq = notify_device->cluster0_freq_floor;
                head_freq = notify_device->cluster0_freq_roof;
    }
     else if (cpumask_test_cpu(policy->cpu, &notify_device->cluster1_cpus))
    {
                limit_freq = notify_device->cluster1_freq_limit;
                base_freq = notify_device->cluster1_freq_floor;
                head_freq = notify_device->cluster1_freq_roof;
    }
    else
            return 0;
    if(limit_freq && limit_freq != INVALID_FREQ)
    {
        max_freq =(head_freq >= limit_freq)?limit_freq:head_freq;
        min_freq = base_freq;
        /* Never exceed policy.max*/
        if (max_freq > policy->max)
              max_freq = policy->max;
        if (min_freq < policy->min)
        {
              min_freq = policy->min;
        }
        min_freq = (min_freq < max_freq)?min_freq:max_freq;
        if (policy->max != max_freq || policy->min != min_freq )
        {
            cpufreq_verify_within_limits(policy, min_freq, max_freq);
			policy->user_policy.max = policy->max;
//            pr_info("CPU Budget:update CPU %d cpufreq max to %lu min to %lu\n",policy->cpu,max_freq, min_freq);
        }
    }
	return 0;
}

/*
 * cpufreq cooling device callback functions are defined below
 */

/**
 * cpu_budget_get_max_state - callback function to get the max cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the max cooling state.
 */
static int cpu_budget_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpu_budget_cooling_device *cpu_budget_device = cdev->devdata;
    *state = cpu_budget_device->tbl_num-1;
    return 0;
}

/**
 * cpu_budget_get_cur_state - callback function to get the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the current cooling state.
 */
static int cpu_budget_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct cpu_budget_cooling_device *cpu_budget_device = cdev->devdata;
	*state = cpu_budget_device->cpu_budget_state;
	return 0;
}

/**
 * cpu_budget_set_cur_state - callback function to set the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: set this variable to the current cooling state.
 */
static int cpu_budget_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct cpu_budget_cooling_device *cpu_budget_device = cdev->devdata;
	return cpu_budget_apply_cooling(cpu_budget_device, state);
}

/* Bind cpufreq callbacks to thermal cooling device ops */
static struct thermal_cooling_device_ops const cpu_budget_cooling_ops = {
	.get_max_state = cpu_budget_get_max_state,
	.get_cur_state = cpu_budget_get_cur_state,
	.set_cur_state = cpu_budget_set_cur_state,
};

/**
 * cpu_budget_cooling_register - function to create cpufreq cooling device.
 * @clip_cpus: cpumask of cpus where the frequency constraints will happen.
 */
struct thermal_cooling_device *cpu_budget_cooling_register(
    struct cpu_budget_table* tbl,   unsigned int tbl_num,
    const struct cpumask *cluster0_cpus,const struct cpumask *cluster1_cpus)
{
	struct thermal_cooling_device *cool_dev;
	struct cpu_budget_cooling_device *cpu_budget_dev = NULL;
	unsigned int min = 0, max = 0;
	char dev_name[THERMAL_NAME_LENGTH];
	int ret = 0, i;
	struct cpufreq_policy policy;

	/*Verify that all the clip cpus have same freq_min, freq_max limit*/
	for_each_cpu(i, cluster0_cpus) {
		/*continue if cpufreq policy not found and not return error*/
		if (!cpufreq_get_policy(&policy, i))
			continue;
		if (min == 0 && max == 0) {
			min = policy.cpuinfo.min_freq;
			max = policy.cpuinfo.max_freq;
		} else {
			if (min != policy.cpuinfo.min_freq ||
				max != policy.cpuinfo.max_freq)
				return ERR_PTR(-EINVAL);
		}
	}

	/*Verify that all the clip cpus have same freq_min, freq_max limit*/
	for_each_cpu(i, cluster1_cpus) {
		/*continue if cpufreq policy not found and not return error*/
		if (!cpufreq_get_policy(&policy, i))
			continue;
		if (min == 0 && max == 0) {
			min = policy.cpuinfo.min_freq;
			max = policy.cpuinfo.max_freq;
		} else {
			if (min != policy.cpuinfo.min_freq ||
				max != policy.cpuinfo.max_freq)
				return ERR_PTR(-EINVAL);
		}
	}

	cpu_budget_dev = kzalloc(sizeof(struct cpu_budget_cooling_device),
			GFP_KERNEL);
	if (!cpu_budget_dev)
		return ERR_PTR(-ENOMEM);
    spin_lock_init(&cpu_budget_dev->lock);
	cpumask_copy(&cpu_budget_dev->cluster0_cpus, cluster0_cpus);
	cpumask_copy(&cpu_budget_dev->cluster1_cpus, cluster1_cpus);
    cpu_budget_dev->tbl = tbl;
    cpu_budget_dev->tbl_num = tbl_num;
	cpu_budget_dev->cluster0_freq_limit = cpu_budget_dev->tbl[0].cluster0_freq;
	cpu_budget_dev->cluster0_num_limit = cpu_budget_dev->tbl[0].cluster0_cpunr;
	cpu_budget_dev->cluster1_freq_limit = cpu_budget_dev->tbl[0].cluster1_freq;
	cpu_budget_dev->cluster1_num_limit = cpu_budget_dev->tbl[0].cluster1_cpunr;
	cpu_budget_dev->gpu_throttle = cpu_budget_dev->tbl[0].gpu_throttle;

	cpu_budget_dev->cluster0_freq_roof = cpu_budget_dev->cluster0_freq_limit;
	cpu_budget_dev->cluster0_num_roof = cpu_budget_dev->cluster0_num_limit;
	cpu_budget_dev->cluster1_freq_roof = cpu_budget_dev->cluster1_freq_limit;
	cpu_budget_dev->cluster1_num_roof = cpu_budget_dev->cluster1_num_limit;

	ret = get_idr(&cpu_budget_idr, &cpu_budget_dev->id);
	if (ret) {
		kfree(cpu_budget_dev);
		return ERR_PTR(-EINVAL);
	}

	sprintf(dev_name, "thermal-budget-%d", cpu_budget_dev->id);
	cool_dev = thermal_cooling_device_register(dev_name, cpu_budget_dev,
						&cpu_budget_cooling_ops);
	if (!cool_dev) {
		release_idr(&cpu_budget_idr, cpu_budget_dev->id);
		kfree(cpu_budget_dev);
		return ERR_PTR(-EINVAL);
	}
	cpu_budget_dev->cool_dev = cool_dev;
	cpu_budget_dev->cpu_budget_state = 0;
	mutex_lock(&cooling_cpu_budget_lock);
	/* Register the notifier for first cpufreq cooling device */
	if (cpu_budget_dev_count == 0)
    {
        pr_info("CPU Budget:Register notifier\n");
        notify_device=cpu_budget_dev;
        cpu_budget_dev->cpufreq_notifer.notifier_call=cpufreq_thermal_notifier;
		cpufreq_register_notifier(&(cpu_budget_dev->cpufreq_notifer),
						CPUFREQ_POLICY_NOTIFIER);
#ifdef CONFIG_HOTPLUG_CPU
        cpu_budget_dev->hotplug_notifer.notifier_call=hotplug_thermal_notifier;
		register_cpu_notifier(&(cpu_budget_dev->hotplug_notifer));
#endif
    }
	cpu_budget_dev_count++;
	mutex_unlock(&cooling_cpu_budget_lock);
    pr_info("CPU Budget:register Success\n");
	return cool_dev;
}
EXPORT_SYMBOL(cpu_budget_cooling_register);

/**
 * cpu_budget_cooling_unregister - function to remove cpufreq_hotplug cooling device.
 * @cdev: thermal cooling device pointer.
 */
void cpu_budget_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct cpu_budget_cooling_device *cpu_budget_dev = cdev->devdata;

	mutex_lock(&cooling_cpu_budget_lock);
	cpu_budget_dev_count--;

	/* Unregister the notifier for the last cpufreq cooling device */
	if (cpu_budget_dev_count == 0) {

		cpufreq_unregister_notifier(&(cpu_budget_dev->cpufreq_notifer),
						CPUFREQ_POLICY_NOTIFIER);
#ifdef CONFIG_HOTPLUG_CPU
		unregister_cpu_notifier(&(cpu_budget_dev->hotplug_notifer));
#endif
	}
	mutex_unlock(&cooling_cpu_budget_lock);

	thermal_cooling_device_unregister(cpu_budget_dev->cool_dev);
	release_idr(&cpu_budget_idr, cpu_budget_dev->id);
	kfree(cpu_budget_dev);
    pr_info("CPU Budget:unregister Success\n");
}
EXPORT_SYMBOL(cpu_budget_cooling_unregister);

