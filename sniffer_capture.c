#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pcap.h>
#include <hiredis/hiredis.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include "common.h"
#include "sniffer_list.h"

struct config {
	const char *program_name;
	pthread_t *thread;
	uint32_t cpu_number;
	uint32_t thr_number;
} sniff_conf;

struct sniff_list sniffer_list;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void error(const char *fmt, ...)
{
	va_list ap;

	(void) fprintf(stderr, "%s: ", sniff_conf.program_name);
	va_start(ap, fmt);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (*fmt) {
		fmt += strlen(fmt);
		if (fmt[-1] != '\n')
			(void) fputc('\n', stderr);
	}

	exit(1);
}

void warning(const char *fmt, ...)
{
    va_list ap;

    (void)fprintf(stderr, "%s: WARNING: ", sniff_conf.program_name);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (*fmt) {
        fmt += strlen(fmt);
        if (fmt[-1] != '\n')
            (void)fputc('\n', stderr);
    }
}

static void print_version(void)
{
	(void) fprintf(stderr, "%s version %s\n", sniff_conf.program_name, VERSION);
	(void) fprintf(stderr, "%s\n", pcap_lib_version());
}

static void print_usage(void)
{
	print_version();
	(void) fprintf(stderr, 
		"Usage: %s [-hv] [-c count]\n"
		"\t\t[-i interface]\n", sniff_conf.program_name);
}

/*
 * Copy arg vector into a new buffer, concatenating arguments with spaces.
 */
char *copy_argv(register char **argv)
{
	register char **p;
	register u_int len = 0;
	char *buf;
	char *src, *dst;

	p = argv;
	if (*p == 0)
		return 0;

	while (*p)
		len += strlen(*p++) + 1;

	buf = (char *)malloc(len);
	if (buf == NULL)
		error("copy_argv: malloc");

	p = argv;
	dst = buf;
	while ((src = *p++) != NULL) {
		while ((*dst++ = *src++) != '\0')
			;
		dst[-1] = ' ';
	}
	dst[-1] = '\0';

	return buf;
}

void sig_handler(int signum)
{
	int i = 0;

	switch (signum) {
		case SIGINT:
		case SIGQUIT:
		case SIGTERM:
			for (i = 0; i < sniff_conf.cpu_number; i++) {
				pthread_cancel(sniff_conf.thread[i]);
			}

			free(sniff_conf.thread);
			sniff_list_destroy();
			break;
	}
}

void *thr_handler(void *arg)
{
	char ip_src[MAXINUM_ADDR_LENGTH] = "";
	char ip_dst[MAXINUM_ADDR_LENGTH] = "";
	char *addr_buf = NULL;
	redisContext *c = NULL;
	int ret = 0;

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

	/* Initialize redis client */
	c = redisConnect("127.0.0.1", 6379);
	if (c->err) {
		error("Connect to redis server failed.\n");
	}

	while (1) {
		memset(ip_src, 0, MAXINUM_ADDR_LENGTH);
		memset(ip_dst, 0, MAXINUM_ADDR_LENGTH);

		struct sniff_iphdr ip_info;
		ret = sniff_list_pull(&ip_info);
		if (ret < 0) {
			continue;
		}

		addr_buf = inet_ntoa(ip_info.src);
		memcpy(ip_src, addr_buf, strlen(addr_buf) + 1);
		addr_buf = inet_ntoa(ip_info.dst);
		memcpy(ip_dst, addr_buf, strlen(addr_buf) + 1);

		u_short total_length = ntohs(ip_info.len);

		redisReply *r = 
			(redisReply *)redisCommand(c, "INCRBY %s,%s %u", ip_src, ip_dst, total_length);
		if (r == NULL) {
			error("execute redis command failed.\n");
		}
		freeReplyObject(r);
	}

	redisFree(c);

	pthread_exit(NULL);
}

void sniffer_handler(u_char *user, 
	const struct pcap_pkthdr *h, const u_char *bytes)
{
	int size_ethernet = sizeof(struct sniff_ethernet);
	const struct sniff_ip *ip_hdr = NULL;
	struct sniff_iphdr ip_info;

	ip_hdr = (struct sniff_ip *) (bytes + size_ethernet);

	ip_info.src = ip_hdr->ip_src;
	ip_info.dst = ip_hdr->ip_dst;
	ip_info.len = ip_hdr->ip_len;

	sniff_list_push(ip_info);
}

void init_config(const char *argv0)
{
	const char *cp = NULL;
	sniff_conf.cpu_number = sysconf(_SC_NPROCESSORS_ONLN);
	sniff_conf.thr_number = sniff_conf.cpu_number - 1;

	if ((cp = strrchr(argv0, '/')) != NULL)
		sniff_conf.program_name = cp + 1;
	else
		sniff_conf.program_name = argv0;

	sniff_conf.thread = (pthread_t *) malloc(sniff_conf.thr_number * sizeof(pthread_t));
	if (sniff_conf.thread == NULL) 
		error("malloc failed.\n");
}

int start_thread(void)
{
	pthread_t *thread = sniff_conf.thread;
	uint32_t thr_number = sniff_conf.thr_number;
	int i = 0; 

	for (i = 0; i < thr_number; i++)
		pthread_create(&thread[i], NULL, thr_handler, NULL);

	return 0;
}

int main(int argc, char **argv)
{
	int opt;
	char *optstring = "i:p:c:hv";
	char *cmd_buf = NULL, *device = NULL;
	int filter_number = -1;
	bpf_u_int32 localnet = 0, netmask = 0;
	struct bpf_program fcode;
	pcap_t *handler = NULL;
	char err_buf[PCAP_ERRBUF_SIZE];
	int i = 0;

	/* register signal handle function */
	#if 0
	signal(SIGINT, sig_handler);
	signal(SIGQUIT, sig_handler);
	signal(SIGTERM, sig_handler);
	#endif

	/* initialize config */
	init_config(argv[0]);

	/* start thread sended redis command */
	start_thread();

	/* parse argument of command line */
	while ((opt = getopt(argc, argv, optstring)) != -1) {
		switch (opt) {
			case 'i':
				device = optarg;
				break;
			case 'c':
				filter_number = atoi(optarg);
				break;
			case 'h':
				print_usage();
				exit(0);
				break;
			case 'v':
				print_version();
				exit(0);
				break;
			default:
				break;
		}
	}

	if (device == NULL) {
		device = pcap_lookupdev(err_buf);
		if (device == NULL) {
			error("%s", err_buf);
		}
	}

	handler = pcap_open_live(device, DEFAULT_SNAPLEN, 0, 0, err_buf);
	if (handler == NULL) 
        error("%s", err_buf);

	if (pcap_lookupnet(device, &localnet, &netmask, err_buf) < 0) {
		error("%s", err_buf);
	}

	if (pcap_datalink(handler) != DLT_EN10MB) {
		error("Device %s doesn't provide Ethernet headers - not supported\n", device);
	}

	cmd_buf = copy_argv(&argv[optind]);
	if (pcap_compile(handler, &fcode, cmd_buf, 1, netmask) < 0)
		error("%s", pcap_geterr(handler));

	if (pcap_setfilter(handler, &fcode) < 0) {
		error("%s", pcap_geterr(handler));
	}

	pcap_loop(handler, filter_number, sniffer_handler, NULL);

	for (i = 0; i < sniff_conf.cpu_number; i++) {
		pthread_cancel(sniff_conf.thread[i]);
	}

    free(sniff_conf.thread);

	pcap_close(handler);

	return 0;
}
