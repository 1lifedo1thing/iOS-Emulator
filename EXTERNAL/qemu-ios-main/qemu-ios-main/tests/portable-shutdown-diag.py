#!/usr/bin/env python3
"""Extract production shutdown diagnostics; --prepare-only never compiles."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    position = opening + 1
    while depth:
        depth += (text[position] == '{') - (text[position] == '}')
        position += 1
    return text[start:position] + '\n'


PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TEST_APPLE
#define __APPLE__ 1
#endif
#include "hw/arm/ipod-shutdown-diagnostics.h"
typedef struct Node { void *data; struct Node *next; } GList;
typedef struct { GList *head; } GQueue;
typedef struct { const char *id; bool dispatched; } AgentItem;
typedef struct {
    int lock; GQueue pending, done; bool claimed; uint64_t token;
    IPodShutdownCancelTrace shutdown_cancel;
} IPodAgent;
static IPodAgent test_agent;
static int locks, frees;
static int64_t now_us = 1000000;
static int64_t g_get_monotonic_time(void) { return now_us; }
static void qemu_mutex_lock(int *m) { assert(!*m); *m=1; ++locks; }
static void qemu_mutex_unlock(int *m) { assert(*m); *m=0; }
static int qemu_mutex_trylock(int *m) { if (*m) return 1; *m=1; return 0; }
static void item_free(void *p) { assert(p); ++frees; }
static void g_queue_delete_link(GQueue *q, GList *node) {
    GList **p=&q->head; while (*p && *p!=node) p=&(*p)->next;
    assert(*p==node); *p=node->next;
}
static void g_queue_clear_full(GQueue *q, void (*release)(void *)) {
    while(q->head) { GList *p=q->head; q->head=p->next; release(p->data); }
}
static bool ready=true, storage=false, running=false, agent_available=true;
static int agent_status=1, request_calls, powerdown_calls, cancellation_calls;
static bool request_success=true;
static bool qemu_ios_ui_ready(void) { return ready; }
static bool qemu_ios_ui_storage_failed(void) { return storage; }
static bool runstate_is_running(void) { return running; }
static int qemu_ios_agent_status(void) { return agent_status; }
static bool qemu_ios_agent_request(const char *text) {
    assert(!strcmp(text,"qemu-ios-shutdown halt\n")); ++request_calls;
    return request_success;
}
static void qemu_ios_ui_powerdown(void) { ++powerdown_calls; }
static void qemu_ios_ui_cancel_input(void) { ++cancellation_calls; }
static IPodAgent *ipod_agent_acquire(void) { return agent_available ? &test_agent : NULL; }
static void ipod_agent_free(IPodAgent *a) { (void)a; }
typedef struct { int code; } Error;
static Error error_value;
static bool cont_error;
static void qmp_cont(Error **error) { if(cont_error) *error=&error_value; else running=true; }
static void qmp_stop(Error **error) { (void)error; running=false; }
static void qmp_system_reset(Error **error) { (void)error; }
static void qmp_system_powerdown(Error **error) { (void)error; }
static void qmp_quit(Error **error) { (void)error; }
static const char *error_get_pretty(Error *error) { assert(error); return "synthetic failure"; }
static void error_free(Error *error) { assert(error); }
static void (*queued)(void *);
static void *queued_opaque;
static void *qemu_get_aio_context(void) { return &queued; }
static void aio_bh_schedule_oneshot(void *context, void (*fn)(void *), void *opaque) {
    assert(context); queued=fn; queued_opaque=opaque;
}
#define qatomic_read(p) (*(p))
#define qatomic_set(p,v) (*(p)=(v))
#define qatomic_inc(p) (++*(p))
static unsigned mock_cmpxchg(unsigned *p,unsigned old,unsigned next) {
    unsigned value=*p; if(value==old)*p=next; return value;
}
#define qatomic_cmpxchg(p,o,n) mock_cmpxchg(p,o,n)
#define smp_wmb() ((void)0)
#define smp_rmb() ((void)0)
static unsigned decision_lines;
static char last_line[768];
static int test_fprintf(FILE *stream,const char *format,...) {
    assert(stream==stderr); va_list args; va_start(args,format);
    int size=vsnprintf(last_line,sizeof(last_line),format,args); va_end(args);
    assert(size>=0 && size<(int)sizeof(last_line));
    if(!strncmp(last_line,"[shutdown-diag]",15))++decision_lines;
    return size;
}
#define fprintf test_fprintf
'''

TESTS = r'''
static void dispatch(void) { assert(queued); void (*fn)(void *)=queued; queued=NULL; fn(queued_opaque); }
int main(void) {
    assert(!ipod_shutdown_diag_flag(NULL) && !ipod_shutdown_diag_flag(""));
    assert(!ipod_shutdown_diag_flag("true") && !ipod_shutdown_diag_flag("10"));
    assert(ipod_shutdown_diag_flag("1"));
    for(unsigned i=0;i<20;++i) assert(ipod_shutdown_diag_in_budget(i)==(i>=1 && i<=8));
    unsetenv("IT_SHUTDOWN_DIAG");
    assert(qemu_ios_ui_shutdown()==1 && request_calls==1 && !decision_lines);
    AgentItem item={"private-id-never-logged",false}; GList node={&item,NULL};
    test_agent.claimed=true; test_agent.token=42; test_agent.pending.head=&node;
    qemu_ios_agent_cancel(item.id);
    assert(!test_agent.pending.head && test_agent.claimed && test_agent.token==42);
    assert(!test_agent.shutdown_cancel.serial);
    setenv("IT_SHUTDOWN_DIAG","1",1);
    item.dispatched=true; test_agent.pending.head=&node;
    qemu_ios_agent_cancel(item.id);
    assert(!test_agent.claimed && test_agent.token==0);
#ifndef TEST_APPLE
    assert(test_agent.shutdown_cancel.reason==IPOD_CANCEL_CLAIM_RESET);
    assert(test_agent.shutdown_cancel.serial==1);
    assert(qemu_ios_ui_shutdown()==1 && decision_lines==1);
    assert(strstr(last_line,"agent=1 agent_present=1 prefer_agent=1 submit_attempt=1 submit_ok=1 route=1"));
    assert(strstr(last_line,"cancel_reason=3") && !strstr(last_line,item.id));
    item.dispatched=false; test_agent.pending.head=&node;
    qemu_ios_agent_cancel(item.id);
    assert(test_agent.shutdown_cancel.reason==IPOD_CANCEL_UNDISPATCHED);
    test_agent.done.head=&node; qemu_ios_agent_cancel(item.id);
    assert(test_agent.shutdown_cancel.reason==IPOD_CANCEL_COMPLETED);
    qemu_ios_agent_cancel(item.id);
    assert(test_agent.shutdown_cancel.reason==IPOD_CANCEL_NOT_FOUND);
    IPodShutdownCancelTrace before=test_agent.shutdown_cancel;
    ipod_agent_cancel(&test_agent,item.id);
    assert(!memcmp(&before,&test_agent.shutdown_cancel,sizeof(before)));
    test_agent.lock=1;
    assert(!ipod_agent_shutdown_trace(&test_agent,&before));
    assert(qemu_ios_ui_shutdown()==1 && strstr(last_line,"cancel_stable=0"));
    test_agent.lock=0;
    schedule_qmp(qmp_cont); assert(!shutdown_cont_serial); dispatch();
    assert(shutdown_cont_serial==1 && shutdown_cont_outcome==1 && shutdown_cont_running);
    now_us+=50000; assert(qemu_ios_ui_shutdown()==1);
    assert(strstr(last_line,"cont_seq=1 cont_result=1 cont_running=1 cont_age_ms=50"));
    cont_error=true; schedule_qmp(qmp_cont); dispatch();
    assert(shutdown_cont_serial==2 && shutdown_cont_outcome==2);
    storage=true; schedule_qmp(qmp_cont); dispatch();
    assert(shutdown_cont_serial==3 && shutdown_cont_outcome==3);
    storage=false; cont_error=false;
    shutdown_cont_revision |= 1;
    assert(qemu_ios_ui_shutdown()==1 && strstr(last_line,"cont_stable=0"));
    shutdown_cont_revision++;
    int before_power=powerdown_calls;
    agent_status=2; assert(qemu_ios_ui_shutdown()==2 && powerdown_calls==before_power+1);
    assert(strstr(last_line,"agent=2 agent_present=1 prefer_agent=0 submit_attempt=0 submit_ok=0 route=2"));
    agent_status=1; request_success=false;
    assert(qemu_ios_ui_shutdown()==2 && strstr(last_line,"submit_attempt=1 submit_ok=0 route=2"));
    ready=false; assert(qemu_ios_ui_shutdown()==0 && strstr(last_line,"ready=0"));
    ready=true; storage=true;
    assert(qemu_ios_ui_shutdown()==0 && strstr(last_line,"storage=1"));
    storage=false;
    assert(decision_lines==8);
    for(unsigned i=0;i<100;++i) assert(qemu_ios_ui_shutdown()==2);
    assert(decision_lines==8 && shutdown_decisions==8);
#else
    assert(!test_agent.shutdown_cancel.serial && !decision_lines);
    schedule_qmp(qmp_cont); dispatch(); assert(!shutdown_cont_serial);
    agent_status=2; assert(qemu_ios_ui_shutdown()==2 && !decision_lines);
#endif
    ipod_agent_reset(&test_agent);
    assert(!test_agent.shutdown_cancel.serial && !test_agent.claimed && !test_agent.token);
    assert(locks && frees && !test_agent.lock);
    return 0;
}
'''


def prepare(output):
    agent = (ROOT / 'hw/arm/ipod-agent.c').read_text()
    extras = (ROOT / 'contrib/macos-app/qemu-macos-extras.c').read_text()
    controls = extras[extras.index('typedef void (*qmp_void_fn)'):extras.index('static void qmp_bh')]
    body = PRELUDE
    for name in ['void ipod_agent_reset(', 'bool ipod_agent_cancel_diagnostic(',
                 'bool ipod_agent_cancel(', 'bool ipod_agent_shutdown_trace(']:
        body += function(agent, name)
    body += controls
    for name in ['static void qmp_bh(', 'static void schedule_qmp(',
                 'void qemu_ios_agent_cancel(', 'static void shutdown_trace_decision(',
                 'int qemu_ios_ui_shutdown(']:
        body += function(extras, name)
    path = output / 'shutdown.c'
    path.write_text(body + TESTS)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = prepare(args.output)
    records = []
    if not args.prepare_only:
        for name, flags in [('portable', []), ('apple', ['-DTEST_APPLE'])]:
            binary = args.output / name
            command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-fsanitize=address,undefined',
                       '-I' + str(ROOT / 'include'), *flags, str(source), '-o', str(binary)]
            for step, argv in [('compile', command), ('run', [str(binary)])]:
                result = subprocess.run(argv, capture_output=True, text=True, timeout=30)
                (args.output / f'{name}-{step}.stdout').write_text(result.stdout)
                (args.output / f'{name}-{step}.stderr').write_text(result.stderr)
                records.append({'variant': name, 'step': step, 'argv': argv, 'exit': result.returncode})
                if result.returncode:
                    raise RuntimeError(f'{name} {step} failed; retained output')
    inputs = ['hw/arm/ipod-agent.c', 'include/hw/arm/ipod-agent.h',
              'include/hw/arm/ipod-shutdown-diagnostics.h', 'contrib/macos-app/qemu-macos-extras.c']
    receipt = {'scope': 'Exact production cancellation/control/decision slices with mock queues/atomics; not concurrency or guest validation',
               'prepare_only': args.prepare_only,
               'inputs': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in inputs},
               'slice_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'commands': records}
    (args.output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
