/*
 * Copyright (C) 2008 Linus Torvalds
 */
#include "git-compat-util.h"
#include "pathspec.h"
#include "dir.h"
#include "environment.h"
#include "fsmonitor.h"
#include "gettext.h"
#include "parse.h"
#include "preload-index.h"
#include "progress.h"
#include "read-cache.h"
#include "thread-utils.h"
#include "repository.h"
#include "symlinks.h"
#include "trace2.h"

/*
 * Mostly randomly chosen maximum thread counts: we
 * cap the parallelism to 20 threads, and we want
 * to have at least 500 lstat's per thread for it to
 * be worth starting a thread.
 */
#define MAX_PARALLEL (60)
#define THREAD_COST (500)

struct progress_data {
	unsigned long n;
	struct progress *progress;
	pthread_mutex_t mutex;
};

struct thread_data {
	pthread_t pthread;
	struct index_state *index;
	struct pathspec pathspec;
	struct progress_data *progress;
	int offset, nr;
	int t2_nr_lstat;
};

static void *preload_thread(void *_data)
{
	int nr, last_nr;
	struct thread_data *p = _data;
	struct index_state *index = p->index;
	struct cache_entry **cep = index->cache + p->offset;
	struct cache_def cache = CACHE_DEF_INIT;

	nr = p->nr;
	if (nr + p->offset > index->cache_nr)
		nr = index->cache_nr - p->offset;
	last_nr = nr;

	do {
		struct cache_entry *ce = *cep++;
		struct stat st;

		if (ce_stage(ce))
			continue;
		if (S_ISGITLINK(ce->ce_mode)) {
			// This call evaluates the submodule HEAD for GITLINK, which really does determine
			// if there is a change (for index purposes). We can't use the traditional path of
			// marking as VALID, because valid can't be used for submodules due to other code
			// paths in which valid may skip investigation of the worktree in the submodule.
			// Gitlinks also aren't statable, or fsmonitorable, so caching doesn't have the same
			// semantics.
			// Use a special entry to mark the ref change state and its validity. Future calls
			// to ce_compare_gitlink will leverage this.
			if (lstat(ce->name, &st))
				continue;
			ce->sub_ref_state = (!!(ie_match_stat(index, ce, &st,
					CE_MATCH_RACY_IS_DIRTY|CE_MATCH_IGNORE_FSMONITOR) & DATA_CHANGED) << 1) | 0x1;
			continue;
		}
		if (ce_uptodate(ce))
			continue;
		if (ce_skip_worktree(ce))
			continue;
		if (ce->ce_flags & CE_FSMONITOR_VALID)
			continue;
		if (p->progress && !(nr & 31)) {
			struct progress_data *pd = p->progress;

			pthread_mutex_lock(&pd->mutex);
			pd->n += last_nr - nr;
			display_progress(pd->progress, pd->n);
			pthread_mutex_unlock(&pd->mutex);
			last_nr = nr;
		}
		if (!ce_path_match(index, ce, &p->pathspec, NULL))
			continue;
		if (threaded_has_symlink_leading_path(&cache, ce->name, ce_namelen(ce)))
			continue;
		p->t2_nr_lstat++;
		if (lstat(ce->name, &st))
			continue;
		if (ie_match_stat(index, ce, &st, CE_MATCH_RACY_IS_DIRTY|CE_MATCH_IGNORE_FSMONITOR))
			continue;
		ce_mark_uptodate(ce);
		mark_fsmonitor_valid(index, ce);
	} while (--nr > 0);
	if (p->progress) {
		struct progress_data *pd = p->progress;

		pthread_mutex_lock(&pd->mutex);
		display_progress(pd->progress, pd->n + last_nr);
		pthread_mutex_unlock(&pd->mutex);
	}
	cache_def_clear(&cache);
	return NULL;
}

void preload_index(struct index_state *index,
		   const struct pathspec *pathspec,
		   unsigned int refresh_flags)
{
	int threads, i, work, offset;
	struct thread_data data[MAX_PARALLEL];
	struct progress_data pd;
	int t2_sum_lstat = 0;
	int link_count = 0;

	if (!HAVE_THREADS || !core_preload_index)
		return;

	for (i = 0; i < index->cache_nr; i++) {
		link_count += (S_ISGITLINK(index->cache[i]->ce_mode));
	}
	// Exploring gitlinks are much more expensive than lstat, so modify the cost
	threads = (index->cache_nr / THREAD_COST) + (link_count / 25);

	if ((index->cache_nr > 1) && (threads < 2) && git_env_bool("GIT_TEST_PRELOAD_INDEX", 0))
		threads = 2;
	if (threads < 2)
		return;

	trace2_region_enter("index", "preload", NULL);

	trace_performance_enter();
	if (threads > MAX_PARALLEL)
		threads = MAX_PARALLEL;
	offset = 0;
	work = DIV_ROUND_UP(index->cache_nr, threads);
	memset(&data, 0, sizeof(data));

	memset(&pd, 0, sizeof(pd));
	if (refresh_flags & REFRESH_PROGRESS && isatty(2)) {
		pd.progress = start_delayed_progress(_("Refreshing index"), index->cache_nr);
		pthread_mutex_init(&pd.mutex, NULL);
	}

	for (i = 0; i < threads; i++) {
		struct thread_data *p = data+i;
		int err;

		p->index = index;
		if (pathspec)
			copy_pathspec(&p->pathspec, pathspec);
		p->offset = offset;
		p->nr = work;
		if (pd.progress)
			p->progress = &pd;
		offset += work;
		err = pthread_create(&p->pthread, NULL, preload_thread, p);

		if (err)
			die(_("unable to create threaded lstat: %s"), strerror(err));
	}
	for (i = 0; i < threads; i++) {
		struct thread_data *p = data+i;
		if (pthread_join(p->pthread, NULL))
			die("unable to join threaded lstat");
		t2_sum_lstat += p->t2_nr_lstat;
	}
	stop_progress(&pd.progress);

	if (pathspec) {
		/* earlier we made deep copies for each thread to work with */
		for (i = 0; i < threads; i++)
			clear_pathspec(&data[i].pathspec);
	}

	trace_performance_leave("preload index");

	trace2_data_intmax("index", NULL, "preload/sum_lstat", t2_sum_lstat);
	trace2_region_leave("index", "preload", NULL);
}

int repo_read_index_preload(struct repository *repo,
			    const struct pathspec *pathspec,
			    unsigned int refresh_flags)
{
	int retval = repo_read_index(repo);

	preload_index(repo->index, pathspec, refresh_flags);
	return retval;
}
