#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include	"common.h"

#include <radcli/radcli.h>

#define BUF_LEN 4096

int process(void *, VALUE_PAIR *, int, int, int);

static void
usage(void)
{

    fprintf(stderr, "usage: radiusclient [-D] [-f config_file] [-p nas_port] [-i] [-e hex-bytes] [-s | [-a] a1=v1 [a2=v2[...[aN=vN]...]]]\n");
    fprintf(stderr, "       -e hex-bytes - Specify an EAP message with colon-separated hex bytes. Ex. -e 2:0:0:9:1:74:65:73:74\n");
    exit(1);
}

int
main(int argc, char **argv)
{
    int i, nas_port, ch, acct, server, ecount, firstline, theend;
    void *rh;
    size_t len;
    VALUE_PAIR *send;
    char *rc_conf, *cp;
    char lbuf[4096];
    int info = 0;
    int debug = 0;
    size_t  eap_len = 0;
    uint8_t eap_msg[255];

    rc_conf = RC_CONFIG_FILE;
    nas_port = 5060;

    acct = 0;
    server = 0;
    while ((ch = getopt(argc, argv, "Daf:p:sie:")) != -1) {
        switch (ch) {
        case 'D':
          debug = 1;
          break;
        case 'f':
            rc_conf = optarg;
            break;

        case 'p':
            nas_port = atoi(optarg);
            break;

        case 'a':
            acct = 1;
            break;

        case 's':
            server = 1;
            break;

        case 'i':
            info = 1;
            break;

        case 'e':
            if (optarg && *optarg != '\0') {
                char   *next = optarg;
                while (*next != '\0') {
                    char    *endptr;
                    long int l = strtol(next, &endptr, 16);
                    if (l > 0xFF) {
                        fprintf(stderr, "-e: hex-bytes invalid. %X greater than 0xFF\n", (unsigned int)l);
                        exit(3);
                    }
                    eap_msg[eap_len++] = (uint8_t)l;
                    if (*endptr == '\0')
                        break;
                    next = endptr + 1;
                }
            } else {
                fprintf(stderr, "-e: can't parse hex-bytes buffer\n");
                exit(3);
            }
            break;

        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if ((argc == 0 && server == 0) || (argc != 0 && server != 0))
        usage();

    if(debug) {
      rc_setdebug(1);
#if HAVE_DECL_LOG_PERROR
      openlog("radiusclient", LOG_PERROR|LOG_NDELAY, LOG_LOCAL7);
#else
      openlog("radiusclient", LOG_NDELAY, LOG_LOCAL7);
#endif
    } else {
      openlog("radiusclient", LOG_NDELAY, LOG_AUTH);
    }

    if ((rh = rc_read_config(rc_conf)) == NULL) {
        fprintf(stderr, "error opening radius configuration file\n");
        exit(1);
    }

    if (rc_read_dictionary(rh, rc_conf_str(rh, "dictionary")) != 0) {
        fprintf(stderr, "error reading radius dictionary\n");
        exit(2);
    }

    if (server == 0) {
        send = NULL;
        for (i = 0; i < argc; i++) {
            if (rc_avpair_parse(rh, argv[i], &send) < 0) {
                fprintf(stderr, "%s: can't parse AV pair\n", argv[i]);
                exit(3);
            }
        }
        if (eap_len > 0) {

            if (rc_avpair_add(rh, &send, PW_EAP_MESSAGE, eap_msg, eap_len, 0) == NULL) {
                fprintf(stderr, "Can't add EAP-Message AV pair\n");
                exit(3);
            }
        }
        exit(process(rh, send, acct, nas_port, info));
    }
    while (1 == 1) {
        send = NULL;
        ecount = 0;
        firstline = 1;
        acct = 0;
        do {
            len = 0;
            cp = rc_fgetln(stdin, &len);
            theend = 1;
            if (cp != NULL && len > 0) {
                if (firstline != 0) {
                    if (len >= 4 && memcmp(cp, "ACCT", 4) == 0)
                        acct = 1;
                    firstline = 0;
                    theend = 0;
                    continue;
                }
                for (i = 0; i < len; i++) {
                    if (!isspace(cp[i])) {
                        theend = 0;
                        break;
                    }
                }
                if (theend == 0) {
                    memcpy(lbuf, cp, len);
                    lbuf[len] = '\0';
                    if (rc_avpair_parse(rh, lbuf, &send) < 0) {
                        fprintf(stderr, "%s: can't parse AV pair\n", lbuf);
                        ecount++;
                    }
                }
            }
        } while (theend == 0);
        if (send != NULL && ecount == 0)
            printf("%d\n\n", process(rh, send, acct, nas_port, info));
        else
            printf("%d\n\n", -1);
        fflush(stdout);
        if (send != NULL)
            rc_avpair_free(send);
		if (cp == NULL || len == 0)
            break;
    }
    exit(0);
}

int
process(void *rh, VALUE_PAIR *send, int acct, int nas_port, int send_info)
{
    VALUE_PAIR *received = NULL;
    char buf[BUF_LEN];
    RC_AAA_CTX *ctx = NULL;
    const unsigned char *p;
    int i, j;

    received = NULL;
    if (acct == 0) {
        i = rc_aaa_ctx(rh, &ctx, nas_port, send, &received, NULL, 1, PW_ACCESS_REQUEST);
        if (received != NULL) {
            printf("%s", rc_avpair_log(rh, received, buf, BUF_LEN));
            rc_avpair_free(received);
        }
        if (ctx) {
	    if (send_info) {
		    printf("Request-Info-Secret = %s\n", rc_aaa_ctx_get_secret(ctx));
		    printf("Request-Info-Vector = ");
		    p = rc_aaa_ctx_get_vector(ctx);
		    for (j=0;j<AUTH_VECTOR_LEN;j++) {
		    	printf("%.2x", (unsigned)p[j]);
		    }
		    printf("\n");
	    }
	    rc_aaa_ctx_free(ctx);
        }
    } else {
        i = rc_acct(rh, nas_port, send);
    }

    return (i == OK_RC) || (i == CHALLENGE_RC) ? 0 : 1;
}
