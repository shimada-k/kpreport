#include <linux/kobject.h>
//#include <linux/string.h>
#include <linux/sysfs.h>
//#include <linux/module.h>
#include <linux/init.h>
#include <linux/rbtree.h>	/* rb_entry, struc rb_node */

/*
	ディレクトリ構造を乱れさせないためのルール
		+ファイルは構造の種類ごとにディレクトリを作ってそこに格納する
		+ディレクトリに分けられるファイルが3つ以上になったらディレクトリを作る
*/


/*
kpreport
	rq_running
	rq_iqr
	+lb
		nr_lb_smt
		nr_lb_mc
		+stat_smt
			this_load
			max_load
			this_load_per_task
				・
				・
				・
		+stat_mc
			this_load
			max_load
			this_load_per_task
				・
				・
				・
*/
static struct kobject *kpreport;
static struct kobject *kpreport_lb;

static ssize_t rq_running(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;
	struct rq *rq[num_processors];

	for(i = 0; i < num_processors; i++){	/* resolution *rq addrres */
		rq[i] = &per_cpu(runqueues, i);
	}

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", rq[i]->nr_running);
	}

	return len;
}

u64 pick_q1_vruntime(struct cfs_rq *cfs_rq)
{
	struct rb_node *node;

	if(!cfs_rq->tasks_timeline.rb_node)
		return 0;

	node = cfs_rq->tasks_timeline.rb_node->rb_left;

	if (!node)
		return 0;

	return rb_entry(node, struct sched_entity, run_node)->vruntime;
}

u64 pick_q3_vruntime(struct cfs_rq *cfs_rq)
{
	struct rb_node *node;

	if(!cfs_rq->tasks_timeline.rb_node)
		return 0;

	node = cfs_rq->tasks_timeline.rb_node->rb_right;

	if (!node)
		return 0;

	return rb_entry(node, struct sched_entity, run_node)->vruntime;
}

u64 load_iqr(int cpu)
{
	struct rq *rq;

	rq = &per_cpu(runqueues, cpu);

	return pick_q3_vruntime(&rq->cfs) - pick_q1_vruntime(&rq->cfs);
}

static ssize_t rq_iqr(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%llu,", load_iqr(i));
	}

	return len;
}

static ssize_t nr_lb_smt(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;
	struct sched_domain *sd;

	for(i = 0; i < num_processors; i++){
		sd = &(per_cpu(cpu_domains, i).sd);
		len += sprintf(buf + len, "%u,",
			sd->lb_count[CPU_NOT_IDLE] + sd->lb_count[CPU_IDLE]
			- (sd->lb_balanced[CPU_NOT_IDLE] + sd->lb_count[CPU_IDLE]));
	}

	return len;
}

static ssize_t nr_lb_mc(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;
	struct sched_domain *sd;

	for(i = 0; i < num_processors; i++){
		sd = &(per_cpu(core_domains, i).sd);
		len += sprintf(buf + len, "%u,",
			sd->lb_count[CPU_NOT_IDLE] + sd->lb_count[CPU_IDLE]
			- (sd->lb_balanced[CPU_NOT_IDLE] + sd->lb_count[CPU_IDLE]));	/* CPU_NOT_IDLEとCPU_IDLE両方調べる */
	}

	return len;
}

static void update_sg_lb_stats_lw(struct sched_group *group, int this_cpu,
			enum cpu_idle_type idle, int load_idx, int *sd_idle,
			int local_group, const struct cpumask *cpus,
			struct sg_lb_stats *sgs)
{
	unsigned long load, max_cpu_load, min_cpu_load;
	int i;
	unsigned int balance_cpu = -1, first_idle_cpu = 0;
	unsigned long sum_avg_load_per_task;
	unsigned long avg_load_per_task;

	/* オリジナルはここにgroup->cpu_powerに値を代入するコードが入る */

	/* Tally up the load of all CPUs in the group */
	sum_avg_load_per_task = avg_load_per_task = 0;
	max_cpu_load = 0;
	min_cpu_load = ~0UL;

	for_each_cpu_and(i, sched_group_cpus(group), cpus) {
		struct rq *rq = cpu_rq(i);

		if (*sd_idle && rq->nr_running)
			*sd_idle = 0;

		/* Bias balancing toward cpus of our domain */
		if (local_group) {
			if (idle_cpu(i) && !first_idle_cpu) {
				first_idle_cpu = 1;
				balance_cpu = i;
			}

			load = target_load(i, load_idx);
		} else {
			load = source_load(i, load_idx);
			if (load > max_cpu_load)
				max_cpu_load = load;
			if (min_cpu_load > load)
				min_cpu_load = load;
		}

		sgs->group_load += load;
		sgs->sum_nr_running += rq->nr_running;
		sgs->sum_weighted_load += weighted_cpuload(i);

		sum_avg_load_per_task += cpu_avg_load_per_task(i);
	}

	/* Adjust by relative CPU power of the group */
	sgs->avg_load = (sgs->group_load * SCHED_LOAD_SCALE) / group->cpu_power;


	/*
	 * Consider the group unbalanced when the imbalance is larger
	 * than the average weight of two tasks.
	 *
	 * APZ: with cgroup the avg task weight can vary wildly and
	 *      might not be a suitable number - should we keep a
	 *      normalized nr_running number somewhere that negates
	 *      the hierarchy?
	 */
	avg_load_per_task = (sum_avg_load_per_task * SCHED_LOAD_SCALE) /
		group->cpu_power;

	if ((max_cpu_load - min_cpu_load) > 2*avg_load_per_task)
		sgs->group_imb = 1;

	sgs->group_capacity =
		DIV_ROUND_CLOSEST(group->cpu_power, SCHED_LOAD_SCALE);

	//printk("avg_load=%lu, group_load=%lu, sum_nr_running=%lu, sum_weighted_load=%lu, group_capacity=%lu\n", sgs->avg_load, sgs->group_load, sgs->sum_nr_running, sgs->sum_weighted_load, sgs->group_capacity);

}

static void update_sd_lb_stats_lw(struct sched_domain *sd, int this_cpu,
			enum cpu_idle_type idle, int *sd_idle,
			const struct cpumask *cpus, struct sd_lb_stats *sds)
{
	struct sched_domain *child = sd->child;
	struct sched_group *group = sd->groups;
	struct sg_lb_stats sgs;
	int load_idx, prefer_sibling = 0;

	if (child && child->flags & SD_PREFER_SIBLING)
		prefer_sibling = 1;

	init_sd_power_savings_stats(sd, sds, idle);	/* sdsメンバに値を代入しているだけ */
	load_idx = get_sd_load_idx(sd, idle);

	do {
		int local_group;

		local_group = cpumask_test_cpu(this_cpu,
					       sched_group_cpus(group));

		/* 構造体をクリア */
		memset(&sgs, 0, sizeof(struct sg_lb_stats));

		update_sg_lb_stats_lw(group, this_cpu, idle, load_idx, sd_idle,
				local_group, cpus, &sgs);

		//printk("local_group=%d, total_load=%lu, total_pwr=%lu\n", local_group, sds->total_load, sds->avg_load);

		//if (local_group){
		//	printk("local_group=%d, total_load=%lu, total_pwr=%lu\n", local_group, sds->total_load, sds->avg_load);
		//	return;
		//}

		sds->total_load += sgs.group_load;
		sds->total_pwr += group->cpu_power;

		/*
		 * In case the child domain prefers tasks go to siblings
		 * first, lower the group capacity to one so that we'll try
		 * and move all the excess tasks away.
		 */
		if (prefer_sibling)
			sgs.group_capacity = min(sgs.group_capacity, 1UL);

		if (local_group) {
			sds->this_load = sgs.avg_load;
			sds->this = group;
			sds->this_nr_running = sgs.sum_nr_running;
			sds->this_load_per_task = sgs.sum_weighted_load;
		} else if (sgs.avg_load > sds->max_load &&
			   (sgs.sum_nr_running > sgs.group_capacity ||
				sgs.group_imb)) {
			sds->max_load = sgs.avg_load;
			sds->busiest = group;
			sds->busiest_nr_running = sgs.sum_nr_running;
			sds->busiest_load_per_task = sgs.sum_weighted_load;
			sds->group_imb = sgs.group_imb;
		}

		update_sd_power_savings_stats(group, sds, local_group, &sgs);
		group = group->next;
	} while (group != sd->groups);
}

#define MAX_DOM_LV	2
static unsigned long sds_last_modify;
struct sd_lb_stats *sds_per_dom;

static void refresh_sds_per_dom(void)
{
	int i;

	int sd_idle = 0;
	struct cpumask cpus;
	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;
	struct sched_domain *sd;

	cpumask_copy(&cpus, cpu_active_mask);

	for(i = 0; i < num_processors; i++){
		int dom_lv = 0;	/* for_each_domainが制御してくれるからこの変数は単純なインクリメントでいい */

		for_each_domain(i, sd){

			memset(&sds[i][dom_lv], 0, sizeof(struct sd_lb_stats));
			update_sd_lb_stats_lw(sd, i, 0, &sd_idle, &cpus, &sds[i][dom_lv]);

			sds[i][dom_lv].avg_load = (SCHED_LOAD_SCALE * sds[i][dom_lv].total_load) / sds[i][dom_lv].total_pwr;
			dom_lv++;
		}
	}

	sds_last_modify = get_seconds();

}

static ssize_t smt_total_load(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].total_load);
	}

	return len;
}

static ssize_t smt_total_pwr(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].total_pwr);
	}

	return len;
}

static ssize_t smt_avg_load(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].avg_load);
	}

	return len;
}

static ssize_t smt_this_load(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].this_load);
	}

	return len;
}

static ssize_t smt_this_load_per_task(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].this_load_per_task);
	}

	return len;
}

static ssize_t smt_this_nr_running(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].this_nr_running);
	}

	return len;
}

static ssize_t smt_max_load(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].max_load);
	}

	return len;
}


static ssize_t smt_busiest_load_per_task(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].busiest_load_per_task);
	}

	return len;
}

static ssize_t smt_busiest_nr_running(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].busiest_nr_running);
	}

	return len;
}

static ssize_t smt_busiest_group_capacity(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].busiest_group_capacity);
	}

	return len;
}

static ssize_t mc_total_load(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].total_load);
	}

	return len;
}

static ssize_t mc_total_pwr(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].total_pwr);
	}

	return len;
}

static ssize_t mc_avg_load(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].avg_load);
	}

	return len;
}

static ssize_t mc_this_load(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].this_load);
	}

	return len;
}

static ssize_t mc_this_load_per_task(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].this_load_per_task);
	}

	return len;
}

static ssize_t mc_this_nr_running(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].this_nr_running);
	}

	return len;
}

static ssize_t mc_max_load(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].max_load);
	}

	return len;
}

static ssize_t mc_busiest_load_per_task(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].busiest_load_per_task);
	}

	return len;
}

static ssize_t mc_busiest_nr_running(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][0].busiest_nr_running);
	}

	return len;
}

static ssize_t mc_busiest_group_capacity(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int i;
	ssize_t len = 0;

	if(get_seconds() - sds_last_modify > 5){
		refresh_sds_per_dom();
	}

	struct sd_lb_stats (*sds)[MAX_DOM_LV] = (struct sd_lb_stats (*)[MAX_DOM_LV])sds_per_dom;

	for(i = 0; i < num_processors; i++){
		len += sprintf(buf + len, "%lu,", sds[i][1].busiest_group_capacity);
	}

	return len;
}

/* kpreport/ */
static struct kobj_attribute rq_running_attr			= __ATTR(rq_running, 0666, rq_running, NULL);
static struct kobj_attribute rq_iqr_attr			= __ATTR(rq_iqr, 0666, rq_iqr, NULL);
/* kpreport/lb/ */
static struct kobj_attribute nr_lb_smt_attr			= __ATTR(nr_lb_smt, 0666, nr_lb_smt, NULL);
static struct kobj_attribute nr_lb_mc_attr			= __ATTR(nr_lb_mc, 0666, nr_lb_mc, NULL);

/* kpreport/stat_smt */
static struct kobj_attribute smt_total_load_attr		= __ATTR(smt_total_load, 0666, smt_total_load, NULL);
static struct kobj_attribute smt_total_pwr_attr			= __ATTR(smt_total_pwr, 0666, smt_total_pwr, NULL);
static struct kobj_attribute smt_avg_load_attr			= __ATTR(smt_avg_load, 0666, smt_avg_load, NULL);
static struct kobj_attribute smt_this_load_attr			= __ATTR(smt_this_load, 0666, smt_this_load, NULL);
static struct kobj_attribute smt_this_load_per_task_attr	= __ATTR(smt_this_load_per_task, 0666, smt_this_load_per_task, NULL);
static struct kobj_attribute smt_this_nr_running_attr		= __ATTR(smt_this_nr_running, 0666, smt_this_nr_running, NULL);
static struct kobj_attribute smt_max_load_attr			= __ATTR(smt_max_load, 0666, smt_max_load, NULL);
static struct kobj_attribute smt_busiest_load_per_task_attr	= __ATTR(smt_busiest_load_per_task, 0666, smt_busiest_load_per_task, NULL);
static struct kobj_attribute smt_busiest_nr_running_attr	= __ATTR(smt_busiest_nr_running, 0666, smt_busiest_nr_running, NULL);
static struct kobj_attribute smt_busiest_group_capacity_attr	= __ATTR(smt_busiest_group_capacity, 0666, smt_busiest_group_capacity, NULL);

/* kpreport/lb/stat_mc */
static struct kobj_attribute mc_total_load_attr			= __ATTR(mc_total_load, 0666, mc_total_load, NULL);
static struct kobj_attribute mc_total_pwr_attr			= __ATTR(mc_total_pwr, 0666, mc_total_pwr, NULL);
static struct kobj_attribute mc_avg_load_attr			= __ATTR(mc_avg_load, 0666, mc_avg_load, NULL);
static struct kobj_attribute mc_this_load_attr			= __ATTR(mc_this_load, 0666, mc_this_load, NULL);
static struct kobj_attribute mc_this_load_per_task_attr		= __ATTR(mc_this_load_per_task, 0666, mc_this_load_per_task, NULL);
static struct kobj_attribute mc_this_nr_running_attr		= __ATTR(mc_this_nr_running, 0666, mc_this_nr_running, NULL);
static struct kobj_attribute mc_max_load_attr			= __ATTR(mc_max_load, 0666, mc_max_load, NULL);
static struct kobj_attribute mc_busiest_load_per_task_attr	= __ATTR(mc_busiest_load_per_task, 0666, mc_busiest_load_per_task, NULL);
static struct kobj_attribute mc_busiest_nr_running_attr		= __ATTR(mc_busiest_nr_running, 0666, mc_busiest_nr_running, NULL);
static struct kobj_attribute mc_busiest_group_capacity_attr	= __ATTR(mc_busiest_group_capacity, 0666, mc_busiest_group_capacity, NULL);


static struct attribute *kpreport_attrs[] = {
	&rq_running_attr.attr,
	&rq_iqr_attr.attr,
       NULL,   /* NULLで終わってないといけない */
};

static struct attribute_group kpreport_attr_group = {
       .attrs = kpreport_attrs,
};


static struct attribute *lb_attrs[] = {
	&nr_lb_smt_attr.attr,
	&nr_lb_mc_attr.attr,
	NULL,	/* NULLで終わってないといけない */
};

static struct attribute_group lb_attr_group = {
	.attrs = lb_attrs,
};

static struct attribute *smt_lb_stat_attrs[] = {
	&smt_total_load_attr.attr,
	&smt_total_pwr_attr.attr,
	&smt_avg_load_attr.attr,
	&smt_this_load_attr.attr,
	&smt_this_load_per_task_attr.attr,
	&smt_this_nr_running_attr.attr,
	&smt_max_load_attr.attr,
	&smt_busiest_load_per_task_attr.attr,
	&smt_busiest_nr_running_attr.attr,
	&smt_busiest_group_capacity_attr.attr,
	NULL,	/* NULLで終わってないといけない */
};

static struct attribute *mc_lb_stat_attrs[] = {
	&mc_total_load_attr.attr,
	&mc_total_pwr_attr.attr,
	&mc_avg_load_attr.attr,
	&mc_this_load_attr.attr,
	&mc_this_load_per_task_attr.attr,
	&mc_this_nr_running_attr.attr,
	&mc_max_load_attr.attr,
	&mc_busiest_load_per_task_attr.attr,
	&mc_busiest_nr_running_attr.attr,
	&mc_busiest_group_capacity_attr.attr,
	NULL,	/* NULLで終わってないといけない */
};

static struct attribute_group stat_smt_attr_group = {
	.name = "stat_smt",
	.attrs = smt_lb_stat_attrs,
};

static struct attribute_group stat_mc_attr_group = {
	.name = "stat_mc",
	.attrs = mc_lb_stat_attrs,
};

static int kpreport_init(void)
{
	int retval;

	sds_per_dom = kzalloc(sizeof(struct sd_lb_stats) * num_processors * MAX_DOM_LV, GFP_KERNEL);

	refresh_sds_per_dom();

	/* /sys/kernel/kpreportの作成 */

	kpreport = kobject_create_and_add("kpreport", kernel_kobj);

	if(!kpreport)
		return -ENOMEM;

	retval = sysfs_create_group(kpreport, &kpreport_attr_group);

	if(retval)
		kobject_put(kpreport);

	/* /sys/kernel/kpreport/lbの作成 */

	kpreport_lb = kobject_create_and_add("lb", kpreport);

	if(!kpreport_lb)
		return -ENOMEM;

	retval = sysfs_create_group(kpreport_lb, &lb_attr_group);

	/* /sys/kernel/kpreport/lb/stat_smt, /sys/kernel/kpreport/lb/stat_mcの作成 */

	retval = sysfs_create_group(kpreport_lb, &stat_smt_attr_group);

	retval = sysfs_create_group(kpreport_lb, &stat_mc_attr_group);

	if(retval)
		kobject_put(kpreport_lb);

	return retval;
}

static void kpreport_exit(void)
{
       kobject_put(kpreport);
}

module_init(kpreport_init);
module_exit(kpreport_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("K.Shimada");

