#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
struct vec{uint64_t*v;size_t n,c;};
static void die(const char*s){perror(s);exit(1);}static uint64_t mono(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))die("clock");return(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}static uint64_t evns(const struct input_event*e){return(uint64_t)e->input_event_sec*1000000000ull+(uint64_t)e->input_event_usec*1000ull;}static void push(struct vec*v,uint64_t x){if(v->n==v->c){size_t c=v->c?v->c*2:8192;void*p=realloc(v->v,c*sizeof(*v->v));if(!p)die("realloc");v->v=p;v->c=c;}v->v[v->n++]=x;}static int cmp(const void*a,const void*b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return(x>y)-(x<y);}static double pct(struct vec*v,double p){if(!v->n)return NAN;qsort(v->v,v->n,sizeof(*v->v),cmp);size_t i=(size_t)llround(p*(double)(v->n-1));return(double)v->v[i]/1000.0;}
int main(int argc,char**argv){if(argc<2||argc>4){fprintf(stderr,"Usage: %s /dev/input/eventN [seconds] [raw.csv]\n",argv[0]);return 2;}double sec=argc>2?strtod(argv[2],0):10.0;FILE*raw=0;if(argc==4){raw=fopen(argv[3],"w");if(!raw)die("fopen");fputs("receive_mono_ns,event_sec,event_usec,type,code,value\n",raw);}int fd=open(argv[1],O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)die("open");uint64_t start=mono(),end=start+(uint64_t)(sec*1e9),events=0,frames=0,rels=0,buttons=0,dups=0,prev=0;struct vec intervals={0};struct pollfd pfd={.fd=fd,.events=POLLIN};while(mono()<end){int rc=poll(&pfd,1,100);if(rc<0&&errno==EINTR)continue;if(rc<0)die("poll");if(!(pfd.revents&POLLIN))continue;struct input_event b[256];ssize_t n=read(fd,b,sizeof(b));if(n<0&&(errno==EAGAIN||errno==EINTR))continue;if(n<0)die("read");if(n%(ssize_t)sizeof(b[0]))die("short record");for(size_t i=0;i<(size_t)n/sizeof(b[0]);i++){struct input_event*e=&b[i];uint64_t r=mono();events++;if(e->type==EV_REL)rels++;if(e->type==EV_KEY)buttons++;if(raw)fprintf(raw,"%"PRIu64",%ld,%ld,%u,%u,%d\n",r,(long)e->input_event_sec,(long)e->input_event_usec,e->type,e->code,e->value);if(e->type==EV_SYN&&e->code==SYN_REPORT){uint64_t t=evns(e);frames++;if(prev){if(t==prev)dups++;else if(t>prev)push(&intervals,t-prev);}prev=t;}}}double elapsed=(double)(mono()-start)/1e9;puts("device,seconds,events,events_per_s,frames,frames_per_s,rel_events,button_events,duplicate_timestamp_frames,p50_frame_interval_us,p99_frame_interval_us,min_frame_interval_us,max_frame_interval_us");printf("%s,%.6f,%"PRIu64",%.3f,%"PRIu64",%.3f,%"PRIu64",%"PRIu64",%"PRIu64",%.3f,%.3f,%.3f,%.3f\n",argv[1],elapsed,events,(double)events/elapsed,frames,(double)frames/elapsed,rels,buttons,dups,pct(&intervals,.5),pct(&intervals,.99),pct(&intervals,0),pct(&intervals,1));if(raw)fclose(raw);close(fd);free(intervals.v);return 0;}
