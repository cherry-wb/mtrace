#include <stdint.h>
#include <string.h>

#include <fcntl.h>

#include <map>
#include <list>

extern "C" {
#include <mtrace-magic.h>
#include "util.h"
#include "objinfo.h"
}

#include "addr2line.hh"
#include "mscan.hh"
#include "dissys.hh"
#include "sersec.hh"

using namespace::std;

typedef map<uint64_t, struct mtrace_label_entry> LabelMap;

// A bunch of global state the default handlers update
struct mtrace_host_entry mtrace_enable;
Addr2line *addr2line;
char mtrace_app_name[32];
MtraceSummary mtrace_summary;
pc_t mtrace_call_pc[MAX_CPUS];
MtraceLabelMap mtrace_label_map;

static LabelMap labels;

class DefaultHostHandler : public EntryHandler {
public:
	virtual void handle(const union mtrace_entry *entry) {
		const struct mtrace_host_entry *e = &entry->host;
		if (e->host_type == mtrace_call_clear_cpu ||
		    e->host_type == mtrace_call_set_cpu) 
		 {
			 return;
		 } else if (e->host_type != mtrace_access_all_cpu)
			die("handle_host: unhandled type %u", e->host_type);
		
		if (!mtrace_app_name[0])
			strncpy(mtrace_app_name, e->access.str, sizeof(mtrace_app_name));
		mtrace_enable = *e;
	}
};

class DefaultAppDataHandler : public EntryHandler {
public:
	virtual void handle(const union mtrace_entry *entry) {
		const struct mtrace_appdata_entry *a = &entry->appdata;
		mtrace_summary.app_ops = a->u64;
	}
};

class DefaultFcallHandler : public EntryHandler {
public:
	virtual void handle(const union mtrace_entry *entry) {
		const struct mtrace_fcall_entry *f = &entry->fcall;
		int cpu = f->h.cpu;

		switch (f->state) {
		case mtrace_resume:
			mtrace_call_pc[cpu] = f->pc;
			break;
		case mtrace_start:
			mtrace_call_pc[cpu] = f->pc;
			break;
		case mtrace_pause:
			mtrace_call_pc[cpu] = 0;
			break;
		case mtrace_done:
			mtrace_call_pc[cpu] = 0;
			break;
		default:
			die("DefaultFcallHandler::handle: default error");
		}
	}
};

class DefaultLabelHandler : public EntryHandler { 
public:
	virtual void handle(const union mtrace_entry *entry) {
		const struct mtrace_label_entry *l = &entry->label;
		
		if (l->label_type == 0 || l->label_type >= mtrace_label_end)
			die("DefaultLabelHandler::handle: bad label type: %u", l->label_type);
		
		if (l->bytes)
			mtrace_label_map.add_label(l);
		else
			mtrace_label_map.rem_label(l);
	}
};

static list<EntryHandler *> entry_handler[mtrace_entry_num];
static list<EntryHandler *> exit_handler;

static inline union mtrace_entry * alloc_entry(void)
{
	return (union mtrace_entry *)malloc(sizeof(union mtrace_entry));
}

static inline void free_entry(void *entry)
{
	free(entry);
}

static inline void init_entry_alloc(void)
{
	// nothing
}

static void process_log(gzFile log)
{
	union mtrace_entry entry;
	int r;

	printf("Scanning log file ...\n");
	fflush(0);
        while ((r = read_entry(log, &entry)) > 0) {
		list<EntryHandler *> *l = &entry_handler[entry.h.type];
		list<EntryHandler *>::iterator it = l->begin();
		for(; it != l->end(); ++it)
			(*it)->handle(&entry);
	}

	list<EntryHandler *>::iterator it = exit_handler.begin();
	for(; it != exit_handler.end(); ++it)
	    (*it)->exit();
}

static void init_handlers(void)
{
	// The default handler come first
	entry_handler[mtrace_entry_host].push_front(new DefaultHostHandler());
	entry_handler[mtrace_entry_appdata].push_front(new DefaultAppDataHandler());
	entry_handler[mtrace_entry_fcall].push_front(new DefaultFcallHandler());
	entry_handler[mtrace_entry_label].push_front(new DefaultLabelHandler());

	//
	// Extra handlers come next
	//
	DistinctSyscalls *dissys = new DistinctSyscalls();
	entry_handler[mtrace_entry_access].push_back(dissys);	
	entry_handler[mtrace_entry_fcall].push_back(dissys);	
	exit_handler.push_back(dissys);

	DistinctOps *disops = new DistinctOps(dissys);
	exit_handler.push_back(disops);

	SerialSections *sersecs = new SerialSections();
	entry_handler[mtrace_entry_lock].push_back(sersecs);
	exit_handler.push_back(sersecs);
}

int main(int ac, char **av)
{
	char symFile[128];
	char elfFile[128];
	char logFile[128];
	gzFile log;
	int symFd;

	if (ac != 3)
		die("usage: %s mtrace-dir mtrace-out", av[0]);

	snprintf(logFile, sizeof(logFile), "%s/%s", av[1], av[2]);
	snprintf(symFile, sizeof(symFile), "%s/vmlinux.syms", av[1]);
	snprintf(elfFile, sizeof(elfFile), "%s/vmlinux", av[1]);

        log = gzopen(logFile, "rb");
        if (!log)
		edie("gzopen %s", logFile);
	if ((symFd = open(symFile, O_RDONLY)) < 0)
		edie("open %s", symFile);

	addr2line = new Addr2line(elfFile);

	init_entry_alloc();
	init_handlers();

	process_log(log);

	gzclose(log);
	close(symFd);
	return 0;
}