/*
 *  Copyright (C) 2002-2006 Nigel Horne <njh@bandsman.co.uk>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */
static	char	const	rcsid[] = "$Id: mbox.c,v 1.380 2007/02/13 19:47:37 njh Exp $";

#ifdef	_MSC_VER
#include <winsock.h>	/* only needed in CL_EXPERIMENTAL */
#endif

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#ifndef	CL_DEBUG
#define	NDEBUG	/* map CLAMAV debug onto standard */
#endif

#ifdef CL_THREAD_SAFE
#ifndef	_REENTRANT
#define	_REENTRANT	/* for Solaris 2.8 */
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#ifdef	HAVE_STRINGS_H
#include <strings.h>
#endif
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#ifdef	HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include "clamav.h"
#ifndef	C_WINDOWS
#include <dirent.h>
#endif
#include <limits.h>
#include <signal.h>

#ifdef	HAVE_UNISTD_H
#include <unistd.h>
#endif

#if defined(HAVE_READDIR_R_3) || defined(HAVE_READDIR_R_2)
#include <stddef.h>
#endif

#ifdef	CL_THREAD_SAFE
#include <pthread.h>
#endif

#include "others.h"
#include "defaults.h"
#include "str.h"
#include "filetypes.h"
#include "mbox.h"

#ifdef	CL_DEBUG

#if	defined(C_LINUX) || defined(C_CYGWIN)
#include <features.h>
#endif

#if __GLIBC__ == 2 && __GLIBC_MINOR__ >= 1
#define HAVE_BACKTRACE
#endif
#endif

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <syslog.h>

static	void	sigsegv(int sig);
static	void	print_trace(int use_syslog);
#endif

#if	defined(NO_STRTOK_R) || !defined(CL_THREAD_SAFE)
#undef strtok_r
#undef __strtok_r
#define strtok_r(a,b,c)	strtok(a,b)
#endif

#ifdef	HAVE_STDBOOL_H
#ifdef	C_BEOS
#include "SupportDefs.h"
#else
#include <stdbool.h>
#endif
#else
#ifdef	FALSE
typedef	unsigned	char	bool;
#else
typedef enum	{ FALSE = 0, TRUE = 1 } bool;
#endif
#endif

typedef	enum {
	FAIL,
	OK,
	OK_ATTACHMENTS_NOT_SAVED,
	VIRUS
} mbox_status;

#ifndef isblank
#define isblank(c)	(((c) == ' ') || ((c) == '\t'))
#endif

#define	SAVE_TO_DISC	/* multipart/message are saved in a temporary file */

#ifndef CL_EXPERIMENTAL
/*
 * Code does exist to run FOLLOWURLS on systems without libcurl, however that
 * is not recommended so it is not compiled by default
 *
 * On Solaris, when using the GNU C compiler, the clamAV build system uses the
 * Sun supplied ld instead of the GNU ld causing an error. Therefore you cannot
 * use WITH_CURL on Solaris with gcc, you must configure with
 * "--without-libcurl". I don't know if it works with Sun's own compiler
 *
 * Fails to link on Solaris 10 with this error:
 *      Undefined			first referenced
 *  symbol				in file
 *  __floatdidf				/opt/sfw/lib/libcurl.s
 */
#if	defined(C_SOLARIS) && defined(__GNUC__)
#undef	WITH_CURL
#endif
#endif

#if	defined(WITH_CURL) || defined(CL_EXPERIMENTAL)
#define	FOLLOWURLS	5	/*
				 * Maximum number of URLs scanned in a message
				 * part. Helps to prevent Dialer.gen-45 and
				 * Trojan.WinREG.Zapchast which are often
				 * dispatched by emails which point to it. If
				 * not defined, don't check any URLs
				 */
#endif

#ifdef	FOLLOWURLS
#include "htmlnorm.h"
#endif

#ifdef CL_EXPERIMENTAL
#include "phishcheck.h"
#endif

#ifdef	FOLLOWURLS

#ifndef	C_WINDOWS
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#ifndef	C_BEOS
#include <net/if.h>
#include <arpa/inet.h>
#endif
#endif

#ifndef	C_WINDOWS
#define	closesocket(s)	close(s)
#endif

#ifdef	CL_EXPERIMENTAL	/* dropping curl support */

#include <fcntl.h>
#ifndef	C_WINDOWS
#include <sys/time.h>
#endif

#ifndef HAVE_IN_PORT_T
typedef	unsigned	short	in_port_t;
#endif

#ifndef HAVE_IN_ADDR_T
typedef	unsigned	int	in_addr_t;
#endif

#if	(!defined(EALREADY)) && (defined(WSAEALREADY))
#define EALREADY	WSAEALREADY
#endif
#if	(!defined(EINPROGRESS)) && (defined(WSAEINPROGRESS))
#define EINPROGRESS	WSAEINPROGRESS
#endif
#if	(!defined(EISCONN)) && (defined(WSAEISCONN))
#define EISCONN	WSAEISCONN
#endif

#else

#ifdef	WITH_CURL	/* Set in configure */
/*
 * To build with WITH_CURL:
 * LDFLAGS=`curl-config --libs` ./configure ...
 */
#include <curl/curl.h>

/*
 * Needs curl >= 7.11 (I've heard that 7.9 can cause crashes and I have seen
 *	7.10 segfault, later versions can be flakey as well)
 * untested)
 *
 * Even 7.15 crashes, valgrind shows this:
 *	==2835== Warning: client switching stacks?  SP change: 0xBEB0FD2C --> 0xD0678F0
*	==2835==          to suppress, use: --max-stackframe=1314225092 or greater

 *	==2835== Invalid write of size 4
 *	==2835==    at 0x40F67BD: Curl_resolv (in /usr/lib/libcurl.so.3.0.0)
 *	==2835==  Address 0xD0678F4 is on thread 1's stack
 *	==2835== Can't extend stack to 0xD067390 during signal delivery for thread 1:
 *	==2835==   no stack segment
 *	==2835==
 *	==2835== Process terminating with default action of signal 11 (SIGSEGV)
 *	==2835==  Access not within mapped region at address 0xD067390
 *	==2835==    at 0x40F67BD: Curl_resolv (in /usr/lib/libcurl.so.3.0.0)
 *
 * This bug has been reported upstream, however they claim that the bug
 *	does not exist :-(. I have received reports that 7.15.5 suffers from the
 *	same problem in Curl_resolv
 *
 * TODO: Drop curl and do it ourselves
 */
#if	(LIBCURL_VERSION_NUM < 0x070B00)
#undef	WITH_CURL	/* also undef FOLLOWURLS? */
#endif

#else
#error	"FOLLOWURLS without CURL is no longer supported"

#endif	/*WITH_CURL*/

#endif	/* CL_EXPERIMENTAL */

#else	/*!FOLLOWURLS*/
#undef	WITH_CURL
#endif	/*FOLLOWURLS*/

/*
 * Define this to handle messages covered by section 7.3.2 of RFC1341.
 *	This is experimental code so it is up to YOU to (1) ensure it's secure
 * (2) periodically trim the directory of old files
 *
 * If you use the load balancing feature of clamav-milter to run clamd on
 * more than one machine you must make sure that .../partial is on a shared
 * network filesystem
 */
#ifndef	C_WINDOWS	/* TODO: when opendir() is done */
#define	PARTIAL_DIR
#endif

/*#define	NEW_WORLD*/

/*#define	SCAN_UNENCODED_BOUNCES	*//*
					 * Slows things down a lot and only catches unencoded copies
					 * of EICAR within bounces, which don't matter
					 */

typedef	struct	mbox_ctx {
	const	char	*dir;
	const	table_t	*rfc821Table;
	const	table_t	*subtypeTable;
	cli_ctx	*ctx;
} mbox_ctx;

static	int	cli_parse_mbox(const char *dir, int desc, cli_ctx *ctx);
static	message	*parseEmailFile(FILE *fin, const table_t *rfc821Table, const char *firstLine, const char *dir);
static	message	*parseEmailHeaders(message *m, const table_t *rfc821Table);
static	int	parseEmailHeader(message *m, const char *line, const table_t *rfc821Table);
static	mbox_status	parseEmailBody(message *messageIn, text *textIn, mbox_ctx *mctx, unsigned int recursion_level);
static	int	boundaryStart(const char *line, const char *boundary);
static	int	endOfMessage(const char *line, const char *boundary);
static	int	initialiseTables(table_t **rfc821Table, table_t **subtypeTable);
static	int	getTextPart(message *const messages[], size_t size);
static	size_t	strip(char *buf, int len);
static	int	parseMimeHeader(message *m, const char *cmd, const table_t *rfc821Table, const char *arg);
static	void	saveTextPart(message *m, const char *dir, int destroy_text);
static	char	*rfc2047(const char *in);
static	char	*rfc822comments(const char *in, char *out);
#ifdef	PARTIAL_DIR
static	int	rfc1341(message *m, const char *dir);
#endif
static	bool	usefulHeader(int commandNumber, const char *cmd);
static	char	*getline_from_mbox(char *buffer, size_t len, FILE *fin);
static	bool	isBounceStart(const char *line);
static	bool	exportBinhexMessage(const char *dir, message *m);
static	int	exportBounceMessage(text *start, const mbox_ctx *ctx);
static	message	*do_multipart(message *mainMessage, message **messages, int i, mbox_status *rc, mbox_ctx *mctx, message *messageIn, text **tptr, unsigned int recursion_level);
static	int	count_quotes(const char *buf);
static	bool	next_is_folded_header(const text *t);
static	bool	newline_in_header(const char *line);

static	void	checkURLs(message *m, mbox_ctx *mctx, mbox_status *rc, int is_html);

#ifdef CL_EXPERIMENTAL
static	void	do_checkURLs(message *m, const char *dir, tag_arguments_t *hrefs);
static	blob	*getHrefs(message *m, tag_arguments_t *hrefs);
static	void	hrefs_done(blob *b, tag_arguments_t *hrefs);
#endif

#if	defined(FOLLOWURLS) && (FOLLOWURLS > 0)
struct arg {
#ifdef	CL_EXPERIMENTAL
	char *url;
#else
	CURL *curl;
	const char *url;
#endif
	const char *dir;
	char *filename;
};
#ifdef	CL_THREAD_SAFE
static	void	*getURL(void *a);
#else
static	void	*getURL(struct arg *arg);
#endif
#endif

/* Maximum line length according to RFC821 */
#define	RFC2821LENGTH	1000

/* Hashcodes for our hash tables */
#define	CONTENT_TYPE			1
#define	CONTENT_TRANSFER_ENCODING	2
#define	CONTENT_DISPOSITION		3

/* Mime sub types */
#define	PLAIN		1
#define	ENRICHED	2
#define	HTML		3
#define	RICHTEXT	4
#define	MIXED		5
#define	ALTERNATIVE	6	/* RFC1521*/
#define	DIGEST		7
#define	SIGNED		8
#define	PARALLEL	9
#define	RELATED		10	/* RFC2387 */
#define	REPORT		11	/* RFC1892 */
#define	APPLEDOUBLE	12	/* Handling of this in only noddy for now */
#define	FAX		MIXED	/*
				 * RFC3458
				 * Drafts stated to treat is as mixed if it is
				 * not known.  This disappeared in the final
				 * version (except when talking about
				 * voice-message), but it is good enough for us
				 * since we do no validation of coversheet
				 * presence etc. (which also has disappeared
				 * in the final version)
				 */
#define	ENCRYPTED	13	/*
				 * e.g. RFC2015
				 * Content-Type: multipart/encrypted;
				 * boundary="nextPart1383049.XCRrrar2yq";
				 * protocol="application/pgp-encrypted"
				 */
#define	X_BFILE		RELATED	/*
				 * BeOS, expert two parts: the file and it's
				 * attributes. The attributes part comes as
				 *	Content-Type: application/x-be_attribute
				 *		name="foo"
				 * I can't find where it is defined, any
				 * pointers would be appreciated. For now
				 * we treat it as multipart/related
				 */
#define	KNOWBOT		14	/* Unknown and undocumented format? */

static	const	struct tableinit {
	const	char	*key;
	int	value;
} rfc821headers[] = {
	/* TODO: make these regular expressions */
	{	"Content-Type",			CONTENT_TYPE		},
	{	"Content-Transfer-Encoding",	CONTENT_TRANSFER_ENCODING	},
	{	"Content-Disposition",		CONTENT_DISPOSITION	},
	{	NULL,				0			}
}, mimeSubtypes[] = {	/* see RFC2045 */
		/* subtypes of Text */
	{	"plain",	PLAIN		},
	{	"enriched",	ENRICHED	},
	{	"html",		HTML		},
	{	"richtext",	RICHTEXT	},
		/* subtypes of Multipart */
	{	"mixed",	MIXED		},
	{	"alternative",	ALTERNATIVE	},
	{	"digest",	DIGEST		},
	{	"signed",	SIGNED		},
	{	"parallel",	PARALLEL	},
	{	"related",	RELATED		},
	{	"report",	REPORT		},
	{	"appledouble",	APPLEDOUBLE	},
	{	"fax-message",	FAX		},
	{	"encrypted",	ENCRYPTED	},
	{	"x-bfile",	X_BFILE		},	/* BeOS */
	{	"knowbot",		KNOWBOT		},	/* ??? */
	{	"knowbot-metadata",	KNOWBOT		},	/* ??? */
	{	"knowbot-code",		KNOWBOT		},	/* ??? */
	{	"knowbot-state",	KNOWBOT		},	/* ??? */
	{	NULL,		0		}
};

#ifdef	CL_THREAD_SAFE
static	pthread_mutex_t	tables_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifndef	O_BINARY
#define	O_BINARY	0
#endif

#ifdef	NEW_WORLD

#include "matcher.h"

#undef	PARTIAL_DIR

#if HAVE_MMAP
#if HAVE_SYS_MMAN_H
#include <sys/mman.h>
#else /* HAVE_SYS_MMAN_H */
#undef HAVE_MMAP
#endif
#else	/*HAVE_MMAP*/
#undef	NEW_WORLD
#endif
#endif

#ifdef	NEW_WORLD
/*
 * Files larger than this are scanned with the old method, should be
 *	StreamMaxLength, I guess
 * If NW_MAX_FILE_SIZE is not defined, all files go through the
 *	new method. This definition is for machines very tight on RAM, or
 *	with large StreamMaxLength values
 */
#define	MAX_ALLOCATION	134217728	/* see libclamav/others.c */
#define	NW_MAX_FILE_SIZE	MAX_ALLOCATION

struct scanlist {
	const	char	*start;
	size_t	size;
	encoding_type	decoder;	/* only BASE64 and QUOTEDPRINTABLE for now */
	struct	scanlist *next;
};

static struct map {
	const	char	*offset;	/* sorted */
	const	char	*word;
	struct	map	*next;
} *map, *tail;

static	int	save_text(cli_ctx *ctx, const char *dir, const char *start, size_t len);
static	void	create_map(const char *begin, const char *end);
static	void	add_to_map(const char *offset, const char *word);
static	const	char	*find_in_map(const char *offset, const char *word);
static	void	free_map(void);

/*
 * This could be the future. Instead of parsing and decoding it just decodes.
 *
 * USE IT AT YOUR PERIL, a large number of viruses are not detected with this
 * method, possibly because the decoded files must be exact and not have
 * extra data at the start or end, which this code will produce.
 *
 * Currently only supports base64 and quoted-printable
 *
 * You may also see a lot of warnings. For the moment it falls back to old
 *	world mode if it doesn't know what to do - that'll be removed.
 * The code is untidy...
 *
 * FIXME: Some mailbox scans are slower with this method. I suspect that it's
 * because the scan can proceed to the end of the file rather than the end
 * of the attachment which can mean than later emails are scanned many times
 *
 * FIXME: quoted printable doesn't know when to stop, so size related virus
 *	matching breaks
 *
 * TODO: Fall through to cli_parse_mbox() too often
 *
 * TODO: Add support for systems without mmap()
 *
 * TODO: partial_dir fall through
 *
 * FIXME: Some EICAR gets through
 */
int
cli_mbox(const char *dir, int desc, cli_ctx *ctx)
{
	char *start, *ptr, *line;
	const char *last, *p, *q;
	size_t size;
	struct stat statb;
	message *m;
	fileblob *fb;
	int ret = CL_CLEAN;
	int wasAlloced;
	struct scanlist *scanlist, *scanelem;

	if(dir == NULL) {
		cli_warnmsg("cli_mbox called with NULL dir\n");
		return CL_ENULLARG;
	}
	if(fstat(desc, &statb) < 0)
		return CL_EOPEN;

	size = statb.st_size;

	if(size == 0)
		return CL_CLEAN;

#ifdef	NW_MAX_FILE_SIZE
	if(size > NW_MAX_FILE_SIZE)
		return cli_parse_mbox(dir, desc, ctx);
#endif

	/*cli_warnmsg("NEW_WORLD is new code - use at your own risk.\n");*/
#ifdef	PARTIAL_DIR
	cli_warnmsg("PARTIAL_DIR doesn't work in the NEW_WORLD yet\n");
#endif

	start = mmap(NULL, size, PROT_READ, MAP_PRIVATE, desc, 0);
	if(start == MAP_FAILED)
		return CL_EMEM;

	cli_dbgmsg("mmap'ed mbox\n");

	ptr = cli_malloc(size);
	if(ptr) {
		memcpy(ptr, start, size);
		munmap(start, size);
		start = ptr;
		wasAlloced = 1;
	} else
		wasAlloced = 0;

	/* last points to the last *valid* address in the array */
	last = &start[size - 1];

	create_map(start, last);

	scanelem = scanlist = NULL;
	q = start;
	/*
	 * FIXME: mismatch of const char * and char * here and in later calls
	 *	to find_in_map()
	 */
	while((p = find_in_map(q, "base64")) != NULL) {
		cli_dbgmsg("Found base64\n");
		if(scanelem) {
			scanelem->next = cli_malloc(sizeof(struct scanlist));
			scanelem = scanelem->next;
		} else
			scanlist = scanelem = cli_malloc(sizeof(struct scanlist));
		scanelem->next = NULL;
		scanelem->decoder = BASE64;
		q = scanelem->start = &p[6];
		if(((p = find_in_map(q, "\nFrom ")) != NULL) ||
		   ((p = find_in_map(q, "base64")) != NULL) ||
		   ((p = find_in_map(q, "quoted-printable")) != NULL)) {
			scanelem->size = (size_t)(p - q);
			q = p;
		} else {
			scanelem->size = (size_t)(last - scanelem->start) + 1;
			break;
		}
		cli_dbgmsg("base64: last %u q %u\n", (unsigned int)last, (unsigned int)q);
		assert(scanelem->size <= size);
	}

	q = start;
	while((p = find_in_map(q, "quoted-printable")) != NULL) {
		if(p != q)
			switch(p[-1]) {
				case ' ':
				case ':':
				case '=':	/* wrong but allow it */
					break;
				default:
					q = &p[16];
					cli_dbgmsg("Ignore quoted-printable false positive\n");
					continue;	/* false positive */
			}

		cli_dbgmsg("Found quoted-printable\n");
#ifdef	notdef
		/*
		 * The problem with quoted printable is recognising when to stop
		 * parsing
		 */
		if(scanelem) {
			scanelem->next = cli_malloc(sizeof(struct scanlist));
			scanelem = scanelem->next;
		} else
			scanlist = scanelem = cli_malloc(sizeof(struct scanlist));
		scanelem->next = NULL;
		scanelem->decoder = QUOTEDPRINTABLE;
		q = scanelem->start = &p[16];
		cli_dbgmsg("qp: last %u q %u\n", (unsigned int)last, (unsigned int)q);
		if(((p = find_in_map(q, "\nFrom ")) != NULL) ||
		   ((p = find_in_map(q, "quoted-printable")) != NULL) ||
		   ((p = find_in_map(q, "base64")) != NULL)) {
			scanelem->size = (size_t)(p - q);
			q = p;
			cli_dbgmsg("qp: scanelem->size = %u\n", scanelem->size);
		} else {
			scanelem->size = (size_t)(last - scanelem->start) + 1;
			break;
		}
		assert(scanelem->size <= size);
#else
		if(wasAlloced)
			free(start);
		else
			munmap(start, size);

		free_map();
		return cli_parse_mbox(dir, desc, ctx);
#endif
	}

	if(scanlist == NULL) {
		const struct tableinit *tableinit;
		bool anyHeadersFound = FALSE;
		bool hasuuencode = FALSE;
		cli_file_t type;

		/* FIXME: message: There could of course be no decoder needed... */
		for(tableinit = rfc821headers; tableinit->key; tableinit++)
			if(find_in_map(start, tableinit->key)) {
				anyHeadersFound = TRUE;
				break;
			}

		if((!anyHeadersFound) &&
		   ((p = find_in_map(start, "\nbegin ")) != NULL) &&
		   (isuuencodebegin(++p)))
			/* uuencoded part */
			hasuuencode = TRUE;
		else {
			cli_dbgmsg("Nothing encoded, looking for a text part to save\n");
			ret = save_text(ctx, dir, start, size);
			if(wasAlloced)
				free(start);
			else
				munmap(start, size);

			free_map();
			if(ret != CL_EFORMAT)
				return ret;
			ret = CL_CLEAN;
		}

		free_map();

		type = cli_filetype(start, size);

		if((type == CL_TYPE_UNKNOWN_TEXT) &&
		   (strncmp(start, "Microsoft Mail Internet Headers", 31) == 0))
			type = CL_TYPE_MAIL;

		if(wasAlloced)
			free(start);
		else
			munmap(start, size);

		if(anyHeadersFound || hasuuencode) {
			/* TODO: reduce the number of falls through here */
			if(hasuuencode)
				/* TODO: fast track visa */
				cli_warnmsg("New world - fall back to old uudecoder\n");
			else
				cli_warnmsg("cli_mbox: unknown encoder, type %d\n", type);
			if(type == CL_TYPE_MAIL)
				return cli_parse_mbox(dir, desc, ctx);
			cli_dbgmsg("Unknown filetype %d, return CLEAN\n", type);
			return CL_CLEAN;
		}

#if	0	/* I don't believe this is needed any more */
		/*
		 * The message could be a plain text phish
		 * FIXME: Can't get to the option whether we are looking for
		 *	phishes or not, so assume we are, this slows things a
		 *	lot
		 * Should be
		 *	if((type == CL_TYPE_MAIL) && (!(no-phishing))
		 */
		if(type == CL_TYPE_MAIL)
			return cli_parse_mbox(dir, desc, ctx);
#endif
		cli_dbgmsg("cli_mbox: I believe it's plain text (type == %d) which must be clean\n",
			type);
		return CL_CLEAN;
	}
#if	0
	if(wasAlloced) {
		const char *max = NULL;

		for(scanelem = scanlist; scanelem; scanelem = scanelem->next) {
			const char *end = &scanelem->start[scanelem->size];

			if(end > max)
				max = end;
		}

		if(max < last)
			printf("could free %d bytes\n", (int)(last - max));
	}
#endif

	for(scanelem = scanlist; scanelem; scanelem = scanelem->next) {
		if(scanelem->decoder == BASE64) {
			const char *b64start = scanelem->start;
			size_t b64size = scanelem->size;

			cli_dbgmsg("b64size = %lu\n", b64size);
			while((*b64start != '\n') && (*b64start != '\r')) {
				b64start++;
				b64size--;
			}
			/*
			 * Look for the end of the headers
			 */
			while(b64start < last) {
				if(*b64start == ';') {
					b64start++;
					b64size--;
				} else if((memcmp(b64start, "\n\n", 2) == 0) ||
					  (memcmp(b64start, "\r\r", 2) == 0)) {
					b64start += 2;
					b64size -= 2;
					break;
				} else if(memcmp(b64start, "\r\n\r\n", 4) == 0) {
					b64start += 4;
					b64size -= 4;
					break;
				} else if(memcmp(b64start, "\n \n", 3) == 0) {
					/*
					 * Some viruses are broken and have
					 * one space character at the end of
					 * the headers
					 */
					b64start += 3;
					b64size -= 3;
					break;
				} else if(memcmp(b64start, "\r\n \r\n", 5) == 0) {
					/*
					 * Some viruses are broken and have
					 * one space character at the end of
					 * the headers
					 */
					b64start += 5;
					b64size -= 5;
					break;
				}
				b64start++;
				b64size--;
			}

			if(b64size > 0L)
				while((!isalnum(*b64start)) && (*b64start != '/')) {
					if(b64size-- == 0L)
						break;
					b64start++;
				}

			if(b64size > 0L) {
				int lastline;
				char *tmpfilename;
				unsigned char *uptr;

				cli_dbgmsg("cli_mbox: decoding %ld base64 bytes\n", b64size);
				if((fb = fileblobCreate()) == NULL) {
					free_map();
					if(wasAlloced)
						free(start);
					else
						munmap(start, size);

					return CL_EMEM;
				}

				tmpfilename = cli_gentemp(dir);
				if(tmpfilename == NULL) {
					free_map();
					if(wasAlloced)
						free(start);
					else
						munmap(start, size);
					fileblobDestroy(fb);

					return CL_EMEM;
				}
				fileblobSetFilename(fb, dir, tmpfilename);
				free(tmpfilename);

				line = NULL;

				m = messageCreate();
				if(m == NULL) {
					free_map();
					if(wasAlloced)
						free(start);
					else
						munmap(start, size);
					fileblobDestroy(fb);

					return CL_EMEM;
				}
				messageSetEncoding(m, "base64");

				messageSetCTX(m, ctx);
				fileblobSetCTX(fb, ctx);

				lastline = 0;
				do {
					int length = 0, datalen;
					char *newline, *equal;
					unsigned char *bigbuf, *data;
					unsigned char smallbuf[1024];
					const char *cptr;

					/*printf("%ld: ", b64size); fflush(stdout);*/

					for(cptr = b64start; b64size && (*cptr != '\n') && (*cptr != '\r'); cptr++) {
						length++;
						--b64size;
					}

					/*printf("%d: ", length); fflush(stdout);*/

					newline = cli_realloc(line, length + 1);
					if(newline == NULL)
						break;
					line = newline;

					memcpy(line, b64start, length);
					line[length] = '\0';

					equal = strchr(line, '=');
					if(equal) {
						lastline++;
						*equal = '\0';
					}
					/*puts(line);*/

#if	0
					if(messageAddStr(m, line) < 0)
						break;
#endif
					if(length >= (int)sizeof(smallbuf)) {
						datalen = length + 2;
						data = bigbuf = cli_malloc(datalen);
						if(data == NULL)
							break;
					} else {
						bigbuf = NULL;
						data = smallbuf;
						datalen = sizeof(data) - 1;
					}
					uptr = decodeLine(m, BASE64, line, data, datalen);

					if(uptr == NULL) {
						if(bigbuf)
							free(bigbuf);
						break;
					}
					/*cli_dbgmsg("base64: write %u bytes\n", (size_t)(uptr - data));*/
					datalen = fileblobAddData(fb, data, (size_t)(uptr - data));
					if(bigbuf)
						free(bigbuf);

					if(datalen < 0)
						break;
					if(fileblobContainsVirus(fb))
						break;

					if((b64size > 0) && (*cptr == '\r')) {
						b64start = ++cptr;
						--b64size;
					}
					if((b64size > 0) && (*cptr == '\n')) {
						b64start = ++cptr;
						--b64size;
					}
					if(lastline)
						break;
				} while(b64size > 0L);

				if(m->base64chars) {
					unsigned char data[4];

					uptr = base64Flush(m, data);
					if(uptr) {
						/*cli_dbgmsg("base64: flush %u bytes\n", (size_t)(uptr - data));*/
						(void)fileblobAddData(fb, data, (size_t)(uptr - data));
					}
				}
				if(fb)
					fileblobDestroy(fb);
				else
					ret = -1;

				messageDestroy(m);
				free(line);
			}
		} else if(scanelem->decoder == QUOTEDPRINTABLE) {
			const char *quotedstart = scanelem->start;
			size_t quotedsize = scanelem->size;

			cli_dbgmsg("quotedsize = %lu\n", quotedsize);
			while(*quotedstart != '\n') {
				quotedstart++;
				quotedsize--;
			}
			/*
			 * Look for the end of the headers
			 */
			while(quotedstart < last) {
				if(*quotedstart == ';') {
					quotedstart++;
					quotedsize--;
				} else if((*quotedstart == '\n') || (*quotedstart == '\r')) {
					quotedstart++;
					quotedsize--;
					if((*quotedstart == '\n') || (*quotedstart == '\r')) {
						quotedstart++;
						quotedsize--;
						break;
					}
				}
				quotedstart++;
				quotedsize--;
			}

			while(!isalnum(*quotedstart)) {
				quotedstart++;
				quotedsize--;
			}

			if(quotedsize > 0L) {
				cli_dbgmsg("cli_mbox: decoding %ld quoted-printable bytes\n", quotedsize);

				m = messageCreate();
				if(m == NULL) {
					free_map();
					if(wasAlloced)
						free(start);
					else
						munmap(start, size);

					return CL_EMEM;
				}
				messageSetEncoding(m, "quoted-printable");
				messageSetCTX(m, ctx);

				line = NULL;

				do {
					int length = 0;
					char *newline;
					const char *cptr;

					/*printf("%ld: ", quotedsize); fflush(stdout);*/

					for(cptr = quotedstart; quotedsize && (*cptr != '\n') && (*cptr != '\r'); cptr++) {
						length++;
						--quotedsize;
					}

					/*printf("%d: ", length); fflush(stdout);*/

					newline = cli_realloc(line, length + 1);
					if(newline == NULL)
						break;
					line = newline;

					memcpy(line, quotedstart, length);
					line[length] = '\0';

					/*puts(line);*/

					if(messageAddStr(m, line) < 0)
						break;

					if((quotedsize > 0) && (*cptr == '\r')) {
						quotedstart = ++cptr;
						--quotedsize;
					}
					if((quotedsize > 0) && (*cptr == '\n')) {
						quotedstart = ++cptr;
						--quotedsize;
					}
				} while(quotedsize > 0L);

				free(line);
				fb = messageToFileblob(m, dir, 1);
				messageDestroy(m);

				if(fb)
					fileblobDestroy(fb);
				else
					ret = -1;
			}
		}
	}
	scanelem = scanlist;

	/*
	 * There could be a phish in the plain text part, so save that
	 * FIXME: Can't get to the option whether we are looking for
	 *	phishes or not, so assume we are, this slows things a
	 *	lot
	 * Should be
	 *	if((type == CL_TYPE_MAIL) && (!(no-phishing))
	 */
	ret = save_text(ctx, dir, start, size);

	free_map();

	while(scanelem) {
		struct scanlist *n = scanelem->next;

		free(scanelem);
		scanelem = n;
	}

	if(wasAlloced)
		free(start);
	else
		munmap(start, size);

	/*
	 * FIXME: Need to run cl_scandir() here and return that value
	 */
	cli_dbgmsg("cli_mbox: ret = %d\n", ret);
	if(ret != CL_EFORMAT)
		return ret;

	cli_warnmsg("New world - don't know what to do - fall back to old world\n");
	/* Fall back for now */
	lseek(desc, 0L, SEEK_SET);
	return cli_parse_mbox(dir, desc, ctx);
}

/*
 * Save a text part - it could contain phish or jscript
 */
static int
save_text(cli_ctx *ctx, const char *dir, const char *start, size_t len)
{
	const char *p;

	if((p = find_in_map(start, "\n\n")) || (p = find_in_map(start, "\r\n\r\n"))) {
		const char *q;
		fileblob *fb;
		char *tmpfilename;

		if(((q = find_in_map(start, "base64")) == NULL) &&
		   ((q = find_in_map(start, "quoted_printable")) == NULL)) {
			cli_dbgmsg("It's all plain text!\n");
			if(*p == '\r')
				p += 4;
			else
				p += 2;
			len -= (p - start);
		} else if(((q = find_in_map(p, "\nFrom ")) == NULL) &&
		   ((q = find_in_map(p, "base64")) == NULL) &&
		   ((q = find_in_map(p, "quoted-printable")) == NULL))
			cli_dbgmsg("Can't find end of plain text - assume it's all\n");
		else
			len = (size_t)(q - p);

		if(len < 5) {
			cli_dbgmsg("save_text: Too small\n");
			return CL_EFORMAT;
		}
		if(ctx->scanned)
			*ctx->scanned += len / CL_COUNT_PRECISION;

		/*
		 * This doesn't work, cli_scanbuff isn't designed to be used
		 *	in this way. It gets the "filetype" wrong and then
		 *	doesn't scan correctly
		 */
		if(cli_scanbuff((char *)p, len, ctx->virname, ctx->engine, CL_TYPE_UNKNOWN_DATA) == CL_VIRUS) {
			cli_dbgmsg("save_text: found %s\n", *ctx->virname);
			return CL_VIRUS;
		}

		fb = fileblobCreate();
		if(fb == NULL)
			return CL_EMEM;

		tmpfilename = cli_gentemp(dir);

		if(tmpfilename == NULL) {
			fileblobDestroy(fb);
			return CL_ETMPFILE;
		}
		cli_dbgmsg("save plain bit to %s, %u bytes\n",
			tmpfilename, len);

		fileblobSetFilename(fb, dir, tmpfilename);
		free(tmpfilename);

		(void)fileblobAddData(fb, (const unsigned char *)p, len);
		fileblobDestroy(fb);
		return CL_SUCCESS;
	}
	cli_dbgmsg("No text part found to save\n");
	return CL_EFORMAT;
}

static void
create_map(const char *begin, const char *end)
{
	const struct wordlist {
		const char *word;
		int len;
	} wordlist[] = {
		{	"base64",		6	},
		{	"quoted-printable",	16	},
		{	"\nbegin ",		7	},
		{	"\nFrom ",		6	},
		{	"\n\n",			2	},
		{	"\r\n\r\n",		4	},
		{	NULL,			0	}
	};

	if(map) {
		cli_warnmsg("create_map called without free_map\n");
		free_map();
	}
	while(begin < end) {
		const struct wordlist *word;

		for(word = wordlist; word->word; word++) {
			if((end - begin) < word->len)
				continue;
			if(strncasecmp(begin, word->word, word->len) == 0) {
				add_to_map(begin, word->word);
				break;
			}
		}
		begin++;
	}
}

/* To sort map, assume 'offset' is presented in sorted order */
static void
add_to_map(const char *offset, const char *word)
{
	if(map) {
		tail->next = cli_malloc(sizeof(struct map));	/* FIXME: verify */
		tail = tail->next;
	} else
		map = tail = cli_malloc(sizeof(struct map));	/* FIXME: verify */

	tail->offset = offset;
	tail->word = word;
	tail->next = NULL;
}

static const char *
find_in_map(const char *offset, const char *word)
{
	const struct map *item;

	for(item = map; item; item = item->next)
		if(item->offset >= offset)
			if(strcasecmp(word, item->word) == 0)
				return item->offset;

	return NULL;
}

static void
free_map(void)
{
	while(map) {
		struct map *next = map->next;

		free(map);
		map = next;
	}
	map = NULL;
}

#else	/*!NEW_WORLD*/
int
cli_mbox(const char *dir, int desc, cli_ctx *ctx)
{
	if(dir == NULL) {
		cli_warnmsg("cli_mbox called with NULL dir\n");
		return CL_ENULLARG;
	}
	return cli_parse_mbox(dir, desc, ctx);
}
#endif

/*
 * TODO: when signal handling is added, need to remove temp files when a
 *	signal is received
 * TODO: add option to scan in memory not via temp files, perhaps with a
 * named pipe or memory mapped file, though this won't work on big e-mails
 * containing many levels of encapsulated messages - it'd just take too much
 * RAM
 * TODO: parse .msg format files
 * TODO: fully handle AppleDouble format, see
 *	http://www.lazerware.com/formats/Specs/AppleSingle_AppleDouble.pdf
 * TODO: ensure parseEmailHeaders is always called before parseEmailBody
 * TODO: create parseEmail which calls parseEmailHeaders then parseEmailBody
 * TODO: Handle unepected NUL bytes in header lines which stop strcmp()s:
 *	e.g. \0Content-Type: application/binary;
 */
static int
cli_parse_mbox(const char *dir, int desc, cli_ctx *ctx)
{
	int retcode, i;
	message *body;
	FILE *fd;
	char buffer[RFC2821LENGTH + 1];
	mbox_ctx mctx;
#ifdef HAVE_BACKTRACE
	void (*segv)(int);
#endif
	static table_t *rfc821, *subtype;
#ifdef	CL_DEBUG
	char tmpfilename[16];
	int tmpfd;
#endif
#if	defined(FOLLOWURLS) && (!defined(CL_EXPERIMENTAL))
	static int initialised = 0;
#ifdef	CL_THREAD_SAFE
	static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
#endif

#ifdef	NEW_WORLD
	cli_dbgmsg("fall back to old world\n");
#else
	cli_dbgmsg("in mbox()\n");
#endif

#if	defined(FOLLOWURLS) && (!defined(CL_EXPERIMENTAL))
	if(ctx->options&CL_SCAN_MAILURL) {
#ifdef	CL_THREAD_SAFE
		pthread_mutex_lock(&init_mutex);
#endif
		if(!initialised) {
			if(curl_global_init(CURL_GLOBAL_ALL) != 0) {
#ifdef	CL_THREAD_SAFE
				pthread_mutex_unlock(&init_mutex);
#endif
				cli_warnmsg("curl_global_init failed, disabling mail-follow-urls");
				ctx->options &= ~CL_SCAN_MAILURL;
			}
			initialised = 1;
		}
#ifdef	CL_THREAD_SAFE
		pthread_mutex_unlock(&init_mutex);
#endif
	}
#endif

	i = dup(desc);
	if((fd = fdopen(i, "rb")) == NULL) {
		cli_errmsg("Can't open descriptor %d\n", desc);
		close(i);
		return CL_EOPEN;
	}
	rewind(fd);	/* bug 240 */
#ifdef	CL_DEBUG
	/*
	 * Copy the incoming mail for debugging, so that if it falls over
	 * we have a copy of the offending email. This is debugging code
	 * that you shouldn't of course install in a live environment. I am
	 * not interested in hearing about security issues with this section
	 * of the parser.
	 */
	strcpy(tmpfilename, "/tmp/mboxXXXXXX");
	tmpfd = mkstemp(tmpfilename);
	if(tmpfd < 0) {
		perror(tmpfilename);
		cli_errmsg("Can't make debugging file\n");
	} else {
		FILE *tmpfp = fdopen(tmpfd, "w");

		if(tmpfp) {
			while(fgets(buffer, sizeof(buffer) - 1, fd) != NULL)
				fputs(buffer, tmpfp);
			fclose(tmpfp);
			rewind(fd);
		} else
			cli_errmsg("Can't fdopen debugging file\n");
	}
#endif
	if(fgets(buffer, sizeof(buffer) - 1, fd) == NULL) {
		/* empty message */
		fclose(fd);
#ifdef	CL_DEBUG
		unlink(tmpfilename);
#endif
		return CL_CLEAN;
	}
#ifdef	CL_THREAD_SAFE
	pthread_mutex_lock(&tables_mutex);
#endif
	if(rfc821 == NULL) {
		assert(subtype == NULL);

		if(initialiseTables(&rfc821, &subtype) < 0) {
			rfc821 = NULL;
			subtype = NULL;
#ifdef	CL_THREAD_SAFE
			pthread_mutex_unlock(&tables_mutex);
#endif
			fclose(fd);
#ifdef	CL_DEBUG
			unlink(tmpfilename);
#endif
			return CL_EMEM;
		}
	}
#ifdef	CL_THREAD_SAFE
	pthread_mutex_unlock(&tables_mutex);
#endif

#ifdef HAVE_BACKTRACE
	segv = signal(SIGSEGV, sigsegv);
#endif

	retcode = CL_SUCCESS;
	body = NULL;

	mctx.dir = dir;
	mctx.rfc821Table = rfc821;
	mctx.subtypeTable = subtype;
	mctx.ctx = ctx;

	/*
	 * Is it a UNIX style mbox with more than one
	 * mail message, or just a single mail message?
	 *
	 * TODO: It would be better if we called cli_scandir here rather than
	 * in cli_scanmail. Then we could improve the way mailboxes with more
	 * than one message is handled, e.g. stopping parsing when an infected
	 * message is stopped, and giving a better indication of which message
	 * within the mailbox is infected
	 */
	/*if((strncmp(buffer, "From ", 5) == 0) && isalnum(buffer[5])) {*/
	if(strncmp(buffer, "From ", 5) == 0) {
		/*
		 * Have been asked to check a UNIX style mbox file, which
		 * may contain more than one e-mail message to decode
		 *
		 * It would be far better for scanners.c to do this splitting
		 * and do this
		 *	FOR EACH mail in the mailbox
		 *	DO
		 *		pass this mail to cli_mbox --
		 *		scan this file
		 *		IF this file has a virus quit
		 *		THEN
		 *			return CL_VIRUS
		 *		FI
		 *	END
		 * This would remove a problem with this code that it can
		 * fill up the tmp directory before it starts scanning
		 */
		bool lastLineWasEmpty;
		int messagenumber;
		message *m = messageCreate();

		if(m == NULL) {
			fclose(fd);
#ifdef HAVE_BACKTRACE
			signal(SIGSEGV, segv);
#endif
#ifdef	CL_DEBUG
			unlink(tmpfilename);
#endif
			return CL_EMEM;
		}

		lastLineWasEmpty = FALSE;
		messagenumber = 1;
		messageSetCTX(m, ctx);

		do {
			cli_chomp(buffer);
			/*if(lastLineWasEmpty && (strncmp(buffer, "From ", 5) == 0) && isalnum(buffer[5])) {*/
			if(lastLineWasEmpty && (strncmp(buffer, "From ", 5) == 0)) {
				cli_dbgmsg("Deal with email number %d\n", messagenumber++);
				/*
				 * End of a message in the mail box
				 */
				body = parseEmailHeaders(m, rfc821);
				if(body == NULL) {
					messageReset(m);
					continue;
				}
				messageSetCTX(body, ctx);
				messageDestroy(m);
				if(messageGetBody(body)) {
					mbox_status rc = parseEmailBody(body, NULL, &mctx, 0);
					if(rc == FAIL) {
						messageReset(body);
						m = body;
						continue;
					} else if(rc == VIRUS) {
						cli_dbgmsg("Message number %d is infected\n",
							messagenumber);
						retcode = CL_VIRUS;
						m = NULL;
						break;
					}
				}
				/*
				 * Starting a new message, throw away all the
				 * information about the old one. It would
				 * be best to be able to scan this message
				 * now, but cli_scanfile needs arguments
				 * that haven't been passed here so it can't be
				 * called
				 */
				m = body;
				messageReset(body);
				messageSetCTX(body, ctx);

				cli_dbgmsg("Finished processing message\n");
			} else
				lastLineWasEmpty = (bool)(buffer[0] == '\0');

			if(isuuencodebegin(buffer)) {
				/*
				 * Fast track visa to uudecode.
				 * TODO: binhex, yenc
				 */
				if(uudecodeFile(m, buffer, dir, fd) < 0)
					if(messageAddStr(m, buffer) < 0)
						break;
			} else
				if(messageAddStr(m, buffer) < 0)
					break;
		} while(fgets(buffer, sizeof(buffer) - 1, fd) != NULL);

		fclose(fd);

		if(retcode == CL_SUCCESS) {
			cli_dbgmsg("Extract attachments from email %d\n", messagenumber);
			body = parseEmailHeaders(m, rfc821);
		}
		if(m)
			messageDestroy(m);
	} else {
		/*
		 * It's a single message, parse the headers then the body
		 */
		if(strncmp(buffer, "P I ", 4) == 0)
			/*
			 * CommuniGate Pro format: ignore headers until
			 * blank line
			 */
			while((fgets(buffer, sizeof(buffer) - 1, fd) != NULL) &&
				(strchr("\r\n", buffer[0]) == NULL))
					;
		/*
		 * Ignore any blank lines at the top of the message
		 */
		while(strchr("\r\n", buffer[0]) &&
		     (getline_from_mbox(buffer, sizeof(buffer) - 1, fd) != NULL))
			;

		buffer[sizeof(buffer) - 1] = '\0';

		body = parseEmailFile(fd, rfc821, buffer, dir);
		fclose(fd);
	}

	if(body) {
		/*
		 * Write out the last entry in the mailbox
		 */
		if((retcode == CL_SUCCESS) && messageGetBody(body)) {
			messageSetCTX(body, ctx);
			switch(parseEmailBody(body, NULL, &mctx, 0)) {
				case FAIL:
					retcode = CL_EFORMAT;
					break;
				case VIRUS:
					retcode = CL_VIRUS;
					break;
			}
		}

		/*
		 * Tidy up and quit
		 */
		messageDestroy(body);
	}

	cli_dbgmsg("cli_mbox returning %d\n", retcode);

#ifdef HAVE_BACKTRACE
	signal(SIGSEGV, segv);
#endif

#ifdef	CL_DEBUG
	unlink(tmpfilename);
#endif
	return retcode;
}

/*
 * Read in an email message from fin, parse it, and return the message
 *
 * FIXME: files full of new lines and nothing else are
 * handled ungracefully...
 */
static message *
parseEmailFile(FILE *fin, const table_t *rfc821, const char *firstLine, const char *dir)
{
	bool inHeader = TRUE;
	bool bodyIsEmpty = TRUE;
	bool lastWasBlank = FALSE, lastBodyLineWasBlank = FALSE;
	message *ret;
	bool anyHeadersFound = FALSE;
	int commandNumber = -1;
	char *fullline = NULL, *boundary = NULL;
	size_t fulllinelength = 0;
	char buffer[RFC2821LENGTH + 1];

	cli_dbgmsg("parseEmailFile\n");

	ret = messageCreate();
	if(ret == NULL)
		return NULL;

	strcpy(buffer, firstLine);
	do {
		const char *line;

		(void)cli_chomp(buffer);

		if(buffer[0] == '\0')
			line = NULL;
		else
			line = buffer;

		/*
		 * Don't blank lines which are only spaces from headers,
		 * otherwise they'll be treated as the end of header marker
		 */
		if(lastWasBlank) {
			lastWasBlank = FALSE;
			if(boundaryStart(buffer, boundary)) {
				cli_dbgmsg("Found a header line with space that should be blank\n");
				inHeader = FALSE;
			}
		}
		if(inHeader) {
			cli_dbgmsg("parseEmailFile: check '%s' fullline %p\n",
				buffer ? buffer : "", fullline);
			if(line && isspace(line[0])) {
				char copy[sizeof(buffer)];

				strcpy(copy, buffer);
				strstrip(copy);
				if(copy[0] == '\0') {
					/*
					 * The header line contains only white
					 * space. This is not the end of the
					 * headers according to RFC2822, but
					 * some MUAs will handle it as though
					 * it were, and virus writers exploit
					 * this bug. We can't just break from
					 * the loop here since that would allow
					 * other exploits such as inserting a
					 * white space line before the
					 * content-type line. So we just have
					 * to make a best guess. Sigh.
					 */
					if(fullline) {
						if(parseEmailHeader(ret, fullline, rfc821) < 0)
							continue;

						free(fullline);
						fullline = NULL;
					}
					if(boundary ||
					   ((boundary = (char *)messageFindArgument(ret, "boundary")) != NULL)) {
						lastWasBlank = TRUE;
						continue;
					}
				}
			}
			if((line == NULL) && (fullline == NULL)) {	/* empty line */
				/*
				 * A blank line signifies the end of
				 * the header and the start of the text
				 */
				if(!anyHeadersFound)
					/* Ignore the junk at the top */
					continue;

				cli_dbgmsg("End of header information\n");
				inHeader = FALSE;
				bodyIsEmpty = TRUE;
			} else {
				char *ptr;
				int lookahead;

				if(fullline == NULL) {
					char cmd[RFC2821LENGTH + 1], out[RFC2821LENGTH + 1];

					/*
					 * Continuation of line we're ignoring?
					 */
					if(isblank(line[0]))
						continue;

					/*
					 * Is this a header we're interested in?
					 */
					if((strchr(line, ':') == NULL) ||
					   (cli_strtokbuf(line, 0, ":", cmd) == NULL)) {
						if(strncmp(line, "From ", 5) == 0)
							anyHeadersFound = TRUE;
						continue;
					}

					ptr = rfc822comments(cmd, out);
					commandNumber = tableFind(rfc821, ptr ? ptr : cmd);

					switch(commandNumber) {
						case CONTENT_TRANSFER_ENCODING:
						case CONTENT_DISPOSITION:
						case CONTENT_TYPE:
							anyHeadersFound = TRUE;
							break;
						default:
							if(!anyHeadersFound)
								anyHeadersFound = usefulHeader(commandNumber, cmd);
							continue;
					}
					fullline = cli_strdup(line);
					fulllinelength = strlen(line) + 1;
				} else if(line != NULL) {
					fulllinelength += strlen(line);
					ptr = cli_realloc(fullline, fulllinelength);
					if(ptr == NULL)
						continue;
					fullline = ptr;
					strcat(fullline, line);
				}

				assert(fullline != NULL);

				lookahead = getc(fin);
				if(lookahead != EOF) {
					ungetc(lookahead, fin);

					/*
					 * Section B.2 of RFC822 says TAB or
					 * SPACE means a continuation of the
					 * previous entry.
					 *
					 * Add all the arguments on the line
					 */
					if(isblank(lookahead))
						continue;
				}

				/*
				 * Handle broken headers, where the next
				 * line isn't indented by whitespace
				 */
				if(fullline[fulllinelength - 2] == ';')
					/* Add arguments to this line */
					continue;

				if(line && (count_quotes(fullline) & 1))
					continue;

				ptr = rfc822comments(fullline, NULL);
				if(ptr) {
					free(fullline);
					fullline = ptr;
				}

				if(parseEmailHeader(ret, fullline, rfc821) < 0)
					continue;

				free(fullline);
				fullline = NULL;
			}
		} else if(line && isuuencodebegin(line)) {
			/*
			 * Fast track visa to uudecode.
			 * TODO: binhex, yenc
			 */
			bodyIsEmpty = FALSE;
			if(uudecodeFile(ret, line, dir, fin) < 0)
				if(messageAddStr(ret, line) < 0)
					break;
		} else {
			if(line == NULL) {
				/*
				 * Although this would save time and RAM, some
				 * phish signatures have been built which need
				 * the blank lines
				 */
				if(lastBodyLineWasBlank &&
				  (messageGetMimeType(ret) != TEXT)) {
					cli_dbgmsg("Ignoring consecutive blank lines in the body\n");
					continue;
				}
				lastBodyLineWasBlank = TRUE;
			} else {
				if(bodyIsEmpty) {
					/*
					 * Broken message: new line in the
					 * middle of the headers, so the first
					 * line of the body is in fact
					 * the last lines of the header
					 */
					if(newline_in_header(line))
						continue;
					bodyIsEmpty = FALSE;
				}
				lastBodyLineWasBlank = FALSE;
			}

			if(messageAddStr(ret, line) < 0)
				break;
		}
	} while(getline_from_mbox(buffer, sizeof(buffer) - 1, fin) != NULL);

	if(boundary)
		free(boundary);

	if(fullline) {
		if(*fullline) switch(commandNumber) {
			case CONTENT_TRANSFER_ENCODING:
			case CONTENT_DISPOSITION:
			case CONTENT_TYPE:
				cli_dbgmsg("parseEmailFile: Fullline unparsed '%s'\n", fullline);
		}
		free(fullline);
	}

	if(!anyHeadersFound) {
		/*
		 * False positive in believing we have an e-mail when we don't
		 */
		messageDestroy(ret);
		cli_dbgmsg("parseEmailFile: no headers found, assuming it isn't an email\n");
		return NULL;
	}

	messageClean(ret);

	cli_dbgmsg("parseEmailFile: return\n");

	return ret;
}

/*
 * The given message contains a raw e-mail.
 *
 * Returns the message's body with the correct arguments set
 *
 * The downside of this approach is that for a short time we have two copies
 * of the message in memory, the upside is that it makes for easier parsing
 * of encapsulated messages, and in the long run uses less memory in those
 * scenarios
 *
 * TODO: remove the duplication with parseEmailFile
 */
static message *
parseEmailHeaders(message *m, const table_t *rfc821)
{
	bool inHeader = TRUE;
	bool bodyIsEmpty = TRUE;
	const text *t;
	message *ret;
	bool anyHeadersFound = FALSE;
	int commandNumber = -1;
	char *fullline = NULL;
	size_t fulllinelength = 0;

	cli_dbgmsg("parseEmailHeaders\n");

	if(m == NULL)
		return NULL;

	ret = messageCreate();

	for(t = messageGetBody(m); t; t = t->t_next) {
		const char *line;

		if(t->t_line)
			line = lineGetData(t->t_line);
		else
			line = NULL;

		if(inHeader) {
			cli_dbgmsg("parseEmailHeaders: check '%s'\n",
				line ? line : "");
			if(line == NULL) {
				/*
				 * A blank line signifies the end of
				 * the header and the start of the text
				 */
				cli_dbgmsg("End of header information\n");
				if(!anyHeadersFound) {
					cli_dbgmsg("Nothing interesting in the header\n");
					break;
				}
				inHeader = FALSE;
				bodyIsEmpty = TRUE;
			} else {
				char *ptr;

				if(fullline == NULL) {
					char cmd[RFC2821LENGTH + 1];

					/*
					 * Continuation of line we're ignoring?
					 */
					if(isblank(line[0]))
						continue;

					/*
					 * Is this a header we're interested in?
					 */
					if((strchr(line, ':') == NULL) ||
					   (cli_strtokbuf(line, 0, ":", cmd) == NULL)) {
						if(strncmp(line, "From ", 5) == 0)
							anyHeadersFound = TRUE;
						continue;
					}

					ptr = rfc822comments(cmd, NULL);
					commandNumber = tableFind(rfc821, ptr ? ptr : cmd);
					if(ptr)
						free(ptr);

					switch(commandNumber) {
						case CONTENT_TRANSFER_ENCODING:
						case CONTENT_DISPOSITION:
						case CONTENT_TYPE:
							anyHeadersFound = TRUE;
							break;
						default:
							if(!anyHeadersFound)
								anyHeadersFound = usefulHeader(commandNumber, cmd);
							continue;
					}
					fullline = cli_strdup(line);
					fulllinelength = strlen(line) + 1;
				} else if(line) {
					fulllinelength += strlen(line);
					ptr = cli_realloc(fullline, fulllinelength);
					if(ptr == NULL)
						continue;
					fullline = ptr;
					strcat(fullline, line);
				}

				assert(fullline != NULL);

				if(next_is_folded_header(t))
					/* Add arguments to this line */
					continue;

				if(count_quotes(fullline) & 1)
					continue;

				ptr = rfc822comments(fullline, NULL);
				if(ptr) {
					free(fullline);
					fullline = ptr;
				}

				if(parseEmailHeader(ret, fullline, rfc821) < 0)
					continue;

				free(fullline);
				fullline = NULL;
			}
		} else {
			if(bodyIsEmpty) {
				if(line == NULL)
					/* throw away leading blank lines */
					continue;
				/*
				 * Broken message: new line in the
				 * middle of the headers, so the first
				 * line of the body is in fact
				 * the last lines of the header
				 */
				if(newline_in_header(line))
					continue;
				bodyIsEmpty = FALSE;
			}
			/*if(t->t_line && isuuencodebegin(t->t_line))
				puts("FIXME: add fast visa here");*/
			/*cli_dbgmsg("Add line to body '%s'\n", line);*/
			if(messageAddLine(ret, t->t_line) < 0)
				break;
		}
	}

	if(fullline) {
		if(*fullline) switch(commandNumber) {
			case CONTENT_TRANSFER_ENCODING:
			case CONTENT_DISPOSITION:
			case CONTENT_TYPE:
				cli_dbgmsg("parseEmailHeaders: Fullline unparsed '%s'\n", fullline);
		}
		free(fullline);
	}

	if(!anyHeadersFound) {
		/*
		 * False positive in believing we have an e-mail when we don't
		 */
		messageDestroy(ret);
		cli_dbgmsg("parseEmailHeaders: no headers found, assuming it isn't an email\n");
		return NULL;
	}

	messageClean(ret);

	cli_dbgmsg("parseEmailHeaders: return\n");

	return ret;
}

/*
 * Handle a header line of an email message
 */
static int
parseEmailHeader(message *m, const char *line, const table_t *rfc821)
{
	char *cmd;
	int ret = -1;
#ifdef CL_THREAD_SAFE
	char *strptr;
#endif
	const char *separater;
	char *copy, tokenseparater[2];

	cli_dbgmsg("parseEmailHeader '%s'\n", line);

	/*
	 * In RFC822 the separater between the key a value is a colon,
	 * e.g.	Content-Transfer-Encoding: base64
	 * However some MUA's are lapse about this and virus writers exploit
	 * this hole, so we need to check all known possiblities
	 */
	for(separater = ":= "; *separater; separater++)
		if(strchr(line, *separater) != NULL)
			break;

	if(*separater == '\0')
		return -1;

	copy = rfc2047(line);
	if(copy == NULL)
		/* an RFC checker would return -1 here */
		copy = cli_strdup(line);

	tokenseparater[0] = *separater;
	tokenseparater[1] = '\0';

#ifdef	CL_THREAD_SAFE
	cmd = strtok_r(copy, tokenseparater, &strptr);
#else
	cmd = strtok(copy, tokenseparater);
#endif

	if(cmd && (strstrip(cmd) > 0)) {
#ifdef	CL_THREAD_SAFE
		char *arg = strtok_r(NULL, "", &strptr);
#else
		char *arg = strtok(NULL, "");
#endif

		if(arg)
			/*
			 * Found a header such as
			 * Content-Type: multipart/mixed;
			 * set arg to be
			 * "multipart/mixed" and cmd to
			 * be "Content-Type"
			 */
			ret = parseMimeHeader(m, cmd, rfc821, arg);
	}
	free(copy);
	return ret;
}

/*
 * This is a recursive routine.
 * FIXME: We are not passed &mrec so we can't check against MAX_MAIL_RECURSION
 *
 * This function parses the body of mainMessage and saves its attachments in dir
 *
 * mainMessage is the buffer to be parsed, it contains an e-mail's body, without
 * any headers. First time of calling it'll be
 * the whole message. Later it'll be parts of a multipart message
 * textIn is the plain text message being built up so far
 */
static mbox_status
parseEmailBody(message *messageIn, text *textIn, mbox_ctx *mctx, unsigned int recursion_level)
{
	mbox_status rc;
	text *aText = textIn;
	message *mainMessage = messageIn;
	fileblob *fb;
	bool infected = FALSE;
#ifdef CL_EXPERIMENTAL
	const int doPhishingScan = mctx->ctx->engine->dboptions&CL_DB_PHISHING_URLS; /* || (mctx->ctx->options&CL_SCAN_PHISHING_GA_TRAIN) || (mctx->ctx->options&CL_SCAN_PHISHING_GA);  kept here for the GA MERGE */
#endif

	cli_dbgmsg("in parseEmailBody\n");

	if(mctx->ctx->limits->maxmailrec) {
		const cli_ctx *ctx = mctx->ctx;	/* needed for BLOCKMAX :-( */

		/*
		 * This is approximate
		 */
		if(recursion_level > ctx->limits->maxmailrec) {

			cli_warnmsg("parseEmailBody: hit maximum recursion level (%u)\n", recursion_level);
			if(BLOCKMAX) {
				if(ctx->virname)
					*ctx->virname = "MIME.RecursionLimit";
				return VIRUS;
			} else
				return OK_ATTACHMENTS_NOT_SAVED;
		}
	}

	rc = OK;

	/* Anything left to be parsed? */
	if(mainMessage && (messageGetBody(mainMessage) != NULL)) {
		mime_type mimeType;
		int subtype, inhead, htmltextPart, inMimeHead, i;
		const char *mimeSubtype;
		char *protocol, *boundary;
		const text *t_line;
		/*bool isAlternative;*/
		message *aMessage;
		int multiparts = 0;
		message **messages = NULL;	/* parts of a multipart message */

		cli_dbgmsg("Parsing mail file\n");

		mimeType = messageGetMimeType(mainMessage);
		mimeSubtype = messageGetMimeSubtype(mainMessage);

		/* pre-process */
		subtype = tableFind(mctx->subtypeTable, mimeSubtype);
		if((mimeType == TEXT) && (subtype == PLAIN)) {
			/*
			 * This is effectively no encoding, notice that we
			 * don't check that charset is us-ascii
			 */
			cli_dbgmsg("assume no encoding\n");
			mimeType = NOMIME;
			messageSetMimeSubtype(mainMessage, "");
		} else if((mimeType == MESSAGE) &&
			  (strcasecmp(mimeSubtype, "rfc822-headers") == 0)) {
			/*
			 * RFC1892/RFC3462: section 2 text/rfc822-headers
			 * incorrectly sent as message/rfc822-headers
			 *
			 * Parse as text/plain, i.e. no mime
			 */
			cli_dbgmsg("Changing message/rfc822-headers to text/rfc822-headers\n");
			mimeType = NOMIME;
			messageSetMimeSubtype(mainMessage, "");
		} else
			cli_dbgmsg("mimeType = %d\n", mimeType);

		switch(mimeType) {
		case NOMIME:
			cli_dbgmsg("Not a mime encoded message\n");
			aText = textAddMessage(aText, mainMessage);
#ifdef CL_EXPERIMENTAL
			if(!doPhishingScan)
				break;
			/*
			 * Fall through: some phishing mails claim they are
			 * text/plain, when they are in fact html
			 */
#else
			break;
#endif
		case TEXT:
			/* text/plain has been preprocessed as no encoding */
#ifdef CL_EXPERIMENTAL
			if((subtype == HTML) || doPhishingScan) {
#else
			if((mctx->ctx->options&CL_SCAN_MAILURL) && (subtype == HTML))
#endif
				/*
				 * It would be better to save and scan the
				 * file and only checkURLs if it's found to be
				 * clean
				 */
				checkURLs(mainMessage, mctx, &rc, (subtype == HTML));
#ifdef CL_EXPERIMENTAL
				/*
				 * There might be html sent without subtype
				 * html too, so scan them for phishing
				 */
				if(rc == VIRUS)
					infected = TRUE;
			}
#endif
			break;
		case MULTIPART:
			cli_dbgmsg("Content-type 'multipart' handler\n");
			boundary = messageFindArgument(mainMessage, "boundary");

			if(boundary == NULL) {
				cli_warnmsg("Multipart/%s MIME message contains no boundary header\n",
					mimeSubtype);
				/* Broken e-mail message */
				mimeType = NOMIME;
				/*
				 * The break means that we will still
				 * check if the file contains a uuencoded file
				 */
				break;
			}

			/* Perhaps it should assume mixed? */
			if(mimeSubtype[0] == '\0') {
				cli_warnmsg("Multipart has no subtype assuming alternative\n");
				mimeSubtype = "alternative";
				messageSetMimeSubtype(mainMessage, "alternative");
			}

			/*
			 * Get to the start of the first message
			 */
			t_line = messageGetBody(mainMessage);

			if(t_line == NULL) {
				cli_warnmsg("Multipart MIME message has no body\n");
				free((char *)boundary);
				mimeType = NOMIME;
				break;
			}

			do
				if(t_line->t_line) {
					if(boundaryStart(lineGetData(t_line->t_line), boundary))
						break;
					/*
					 * Found a binhex file before
					 *	the first multipart
					 * TODO: check yEnc
					 */
					if(binhexBegin(mainMessage) == t_line) {
						if(exportBinhexMessage(mctx->dir, mainMessage)) {
							/* virus found */
							rc = VIRUS;
							infected = TRUE;
							break;
						}
					} else if(t_line->t_next &&
						 (encodingLine(mainMessage) == t_line->t_next)) {
						/*
						 * We look for the next line
						 * since later on we'll skip
						 * over the important line when
						 * we think it's a blank line
						 * at the top of the message -
						 * which it would have been in
						 * an RFC compliant world
						 */
						cli_dbgmsg("Found MIME attachment before the first MIME section \"%s\"\n",
							lineGetData(t_line->t_next->t_line));
						if(messageGetEncoding(mainMessage) == NOENCODING)
							break;
					}
				}
			while((t_line = t_line->t_next) != NULL);

			if(t_line == NULL) {
				cli_dbgmsg("Multipart MIME message contains no boundary lines (%s)\n",
					boundary);
				/*
				 * Free added by Thomas Lamy
				 * <Thomas.Lamy@in-online.net>
				 */
				free((char *)boundary);
				mimeType = NOMIME;
				/*
				 * The break means that we will still
				 * check if the file contains a yEnc/binhex file
				 */
				break;
			}
			/*
			 * Build up a table of all of the parts of this
			 * multipart message. Remember, each part may itself
			 * be a multipart message.
			 */
			inhead = 1;
			inMimeHead = 0;

			/*
			 * Re-read this variable in case mimeSubtype has changed
			 */
			subtype = tableFind(mctx->subtypeTable, mimeSubtype);

			/*
			 * Parse the mainMessage object and create an array
			 * of objects called messages, one for each of the
			 * multiparts that mainMessage contains.
			 *
			 * This looks like parseEmailHeaders() - maybe there's
			 * some duplication of code to be cleaned up
			 *
			 * We may need to create an array rather than just
			 * save each part as it is found because not all
			 * elements will need scanning, and we don't yet know
			 * which of those elements it will be, except in
			 * the case of mixed, when all parts need to be scanned.
			 */
			for(multiparts = 0; t_line && !infected; multiparts++) {
				int lines = 0;
				message **m;

				m = cli_realloc(messages, ((multiparts + 1) * sizeof(message *)));
				if(m == NULL)
					break;
				messages = m;

				aMessage = messages[multiparts] = messageCreate();
				if(aMessage == NULL) {
					multiparts--;
					continue;
				}
				messageSetCTX(aMessage, mctx->ctx);

				cli_dbgmsg("Now read in part %d\n", multiparts);

				/*
				 * Ignore blank lines. There shouldn't be ANY
				 * but some viruses insert them
				 */
				while((t_line = t_line->t_next) != NULL)
					if(t_line->t_line &&
					   /*(cli_chomp(t_line->t_text) > 0))*/
					   (strlen(lineGetData(t_line->t_line)) > 0))
						break;

				if(t_line == NULL) {
					cli_dbgmsg("Empty part\n");
					/*
					 * Remove this part unless there's
					 * a binhex portion somewhere in
					 * the complete message that we may
					 * throw away by mistake if the MIME
					 * encoding information is incorrect
					 */
					if(mainMessage &&
					   (binhexBegin(mainMessage) == NULL)) {
						messageDestroy(aMessage);
						--multiparts;
					}
					continue;
				}

				do {
					const char *line = lineGetData(t_line->t_line);

					/*cli_dbgmsg("multipart %d: inMimeHead %d inhead %d boundary '%s' line '%s' next '%s'\n",
						multiparts, inMimeHead, inhead, boundary, line,
						t_line->t_next && t_line->t_next->t_line ? lineGetData(t_line->t_next->t_line) : "(null)");*/

					if(inMimeHead) {	/* continuation line */
						if(line == NULL) {
							/*inhead =*/ inMimeHead = 0;
							continue;
						}
						/*
						 * Handle continuation lines
						 * because the previous line
						 * ended with a ; or this line
						 * starts with a white space
						 */
						cli_dbgmsg("Multipart %d: About to add mime Argument '%s'\n",
							multiparts, line);
						/*
						 * Handle the case when it
						 * isn't really a continuation
						 * line:
						 * Content-Type: application/octet-stream;
						 * Content-Transfer-Encoding: base64
						 */
						parseEmailHeader(aMessage, line, mctx->rfc821Table);

						while(isspace((int)*line))
							line++;

						if(*line == '\0') {
							inhead = inMimeHead = 0;
							continue;
						}
						inMimeHead = FALSE;
						messageAddArgument(aMessage, line);
					} else if(inhead) {	/* handling normal headers */
						/*int quotes;*/
						char *fullline, *ptr;

						if(line == NULL) {
							/*
							 * empty line, should the end of the headers,
							 * but some base64 decoders, e.g. uudeview, are broken
							 * and will handle this type of entry, decoding the
							 * base64 content...
							 * Content-Type: application/octet-stream; name=text.zip
							 * Content-Transfer-Encoding: base64
							 * Content-Disposition: attachment; filename="text.zip"
							 *
							 * Content-Disposition: attachment;
							 *	filename=text.zip
							 * Content-Type: application/octet-stream;
							 *	name=text.zip
							 * Content-Transfer-Encoding: base64
							 *
							 * UEsDBAoAAAAAAACgPjJ2RHw676gAAO+oAABEAAAAbWFpbF90ZXh0LWluZm8udHh0ICAgICAgICAg
							 */
							const text *next = t_line->t_next;

							if(next && next->t_line) {
								const char *data = lineGetData(next->t_line);

								if((messageGetEncoding(aMessage) == NOENCODING) &&
								   (messageGetMimeType(aMessage) == APPLICATION) &&
								   strstr(data, "base64")) {
									/*
									 * Handle this nightmare (note the blank
									 * line in the header and the incorrect
									 * content-transfer-encoding header)
									 *
									 * Content-Type: application/octet-stream; name="zipped_files.EXEX-Spanska: Yes
									 *
									 * r-Encoding: base64
									 * Content-Disposition: attachment; filename="zipped_files.EXE"
									 */
									messageSetEncoding(aMessage, "base64");
									cli_dbgmsg("Ignoring fake end of headers\n");
									continue;
								}
								if((strncmp(data, "Content", 7) == 0) ||
								   (strncmp(data, "filename=", 9) == 0)) {
									cli_dbgmsg("Ignoring fake end of headers\n");
									continue;
								}
							}
							cli_dbgmsg("Multipart %d: End of header information\n",
								multiparts);
							inhead = 0;
							continue;
						}
						if(isspace((int)*line)) {
							/*
							 * The first line is
							 * continuation line.
							 * This is tricky
							 * to handle, but
							 * all we can do is our
							 * best
							 */
							cli_dbgmsg("Part %d starts with a continuation line\n",
								multiparts);
							messageAddArgument(aMessage, line);
							/*
							 * Give it a default
							 * MIME type since
							 * that may be the
							 * missing line
							 *
							 * Choose application to
							 * force a save
							 */
							if(messageGetMimeType(aMessage) == NOMIME)
								messageSetMimeType(aMessage, "application");
							continue;
						}

						inMimeHead = FALSE;

						assert(strlen(line) <= RFC2821LENGTH);

						fullline = rfc822comments(line, NULL);
						if(fullline == NULL)
							fullline = cli_strdup(line);

						/*quotes = count_quotes(fullline);*/

						/*
						 * Fold next lines to the end of this
						 * if they start with a white space
						 * or if this line has an odd number of quotes:
						 * Content-Type: application/octet-stream; name="foo
						 * "
						 */
						while(t_line && next_is_folded_header(t_line)) {
							const char *data;

							t_line = t_line->t_next;

							data = lineGetData(t_line->t_line);

							if(data[1] == '\0') {
								/*
								 * Broken message: the
								 * blank line at the end
								 * of the headers isn't blank -
								 * it contains a space
								 */
								cli_dbgmsg("Multipart %d: headers not terminated by blank line\n",
									multiparts);
								inhead = FALSE;
								break;
							}

							ptr = cli_realloc(fullline,
								strlen(fullline) + strlen(data) + 1);

							if(ptr == NULL)
								break;

							fullline = ptr;
							strcat(fullline, data);

							/*quotes = count_quotes(data);*/
						}

						cli_dbgmsg("Multipart %d: About to parse folded header '%s'\n",
							multiparts, fullline);

						parseEmailHeader(aMessage, fullline, mctx->rfc821Table);
						free(fullline);
					} else if(endOfMessage(line, boundary)) {
						/*
						 * Some viruses put information
						 * *after* the end of message,
						 * which presumably some broken
						 * mail clients find, so we
						 * can't assume that this
						 * is the end of the message
						 */
						/* t_line = NULL;*/
						break;
					} else if(boundaryStart(line, boundary)) {
						inhead = 1;
						break;
					} else {
						if(messageAddLine(aMessage, t_line->t_line) < 0)
							break;
						lines++;
					}
				} while((t_line = t_line->t_next) != NULL);

				cli_dbgmsg("Part %d has %d lines\n",
					multiparts, lines);

				/*
				 * Only save in the array of messages if some
				 * decision will be taken on whether to scan.
				 * If all parts will be scanned then save to
				 * file straight away
				 */
				switch(subtype) {
					case MIXED:
					case ALTERNATIVE:
					case REPORT:
					case DIGEST:
					case APPLEDOUBLE:
					case KNOWBOT:
					case -1:
						mainMessage = do_multipart(mainMessage,
							messages, multiparts,
							&rc, mctx, messageIn,
							&aText, recursion_level);
						--multiparts;
						if(rc == VIRUS)
							infected = TRUE;
						break;
					default:
						messageClean(aMessage);
				}
			}

			free((char *)boundary);

			/*
			 * Preprocess. Anything special to be done before
			 * we handle the multiparts?
			 */
			switch(subtype) {
				case KNOWBOT:
					/* TODO */
					cli_dbgmsg("multipart/knowbot parsed as multipart/mixed for now\n");
					mimeSubtype = "mixed";
					break;
				case -1:
					/*
					 * According to section 7.2.6 of
					 * RFC1521, unrecognised multiparts
					 * should be treated as multipart/mixed.
					 */
					cli_dbgmsg("Unsupported multipart format `%s', parsed as mixed\n", mimeSubtype);
					mimeSubtype = "mixed";
					break;
			}

			/*
			 * We've finished message we're parsing
			 */
			if(mainMessage && (mainMessage != messageIn)) {
				messageDestroy(mainMessage);
				mainMessage = NULL;
			}

			cli_dbgmsg("The message has %d parts\n", multiparts);

			if(((multiparts == 0) || infected) && (aText == NULL)) {
				if(messages) {
					for(i = 0; i < multiparts; i++)
						if(messages[i])
							messageDestroy(messages[i]);
					free(messages);
				}

				/*
				 * FIXME: we could return 2 here when we have
				 * saved stuff earlier
				 *
				 * Nothing to do
				 */
				return (rc == VIRUS) ? VIRUS : OK_ATTACHMENTS_NOT_SAVED;
			}

			cli_dbgmsg("Find out the multipart type (%s)\n", mimeSubtype);

			/*
			 * We now have all the parts of the multipart message
			 * in the messages array:
			 *	message *messages[multiparts]
			 * Let's decide what to do with them all
			 */
			switch(tableFind(mctx->subtypeTable, mimeSubtype)) {
			case RELATED:
				cli_dbgmsg("Multipart related handler\n");
				/*
				 * Have a look to see if there's HTML code
				 * which will need scanning
				 */
				aMessage = NULL;
				assert(multiparts > 0);

				htmltextPart = getTextPart(messages, multiparts);

				if(htmltextPart >= 0)
					aText = textAddMessage(aText, messages[htmltextPart]);
				else
					/*
					 * There isn't an HTML bit. If there's a
					 * multipart bit, it'll may be in there
					 * somewhere
					 */
					for(i = 0; i < multiparts; i++)
						if(messageGetMimeType(messages[i]) == MULTIPART) {
							aMessage = messages[i];
							htmltextPart = i;
							break;
						}

				if(htmltextPart == -1)
					cli_dbgmsg("No HTML code found to be scanned\n");
				else {
					rc = parseEmailBody(aMessage, aText, mctx, recursion_level + 1);
					if(rc == OK) {
						assert(aMessage == messages[htmltextPart]);
						messageDestroy(aMessage);
						messages[htmltextPart] = NULL;
					}
				}

				/*
				 * Fixed based on an idea from Stephen White <stephen@earth.li>
				 * The message is confused about the difference
				 * between alternative and related. Badtrans.B
				 * suffers from this problem.
				 *
				 * Fall through in this case:
				 * Content-Type: multipart/related;
				 *	type="multipart/alternative"
				 */
				/*
				 * Changed to always fall through based on
				 * an idea from Michael Dankov <misha@btrc.ru>
				 * that some viruses are completely confused
				 * about the difference between related
				 * and mixed
				 */
				/*cptr = messageFindArgument(mainMessage, "type");
				if(cptr == NULL)
					break;
				isAlternative = (bool)(strcasecmp(cptr, "multipart/alternative") == 0);
				free((char *)cptr);
				if(!isAlternative)
					break;*/
			case DIGEST:
				/*
				 * According to section 5.1.5 RFC2046, the
				 * default mime type of multipart/digest parts
				 * is message/rfc822
				 *
				 * We consider them as alternative, wrong in
				 * the strictest sense since they aren't
				 * alternatives - all parts a valid - but it's
				 * OK for our needs since it means each part
				 * will be scanned
				 */
			case ALTERNATIVE:
				cli_dbgmsg("Multipart alternative handler\n");

				/*
				 * Fall through - some clients are broken and
				 * say alternative instead of mixed. The Klez
				 * virus is broken that way, and anyway we
				 * wish to scan all of the alternatives
				 */
			case REPORT:
				/*
				 * According to section 1 of RFC1892, the
				 * syntax of multipart/report is the same
				 * as multipart/mixed. There are some required
				 * parameters, but there's no need for us to
				 * verify that they exist
				 */
			case MIXED:
			case APPLEDOUBLE:	/* not really supported */
				/*
				 * Look for attachments
				 *
				 * Not all formats are supported. If an
				 * unsupported format turns out to be
				 * common enough to implement, it is a simple
				 * matter to add it
				 */
				if(aText) {
					if(mainMessage && (mainMessage != messageIn))
						messageDestroy(mainMessage);
					mainMessage = NULL;
				}

				cli_dbgmsg("Mixed message with %d parts\n", multiparts);
				for(i = 0; i < multiparts; i++) {
					mainMessage = do_multipart(mainMessage,
						messages, i, &rc, mctx,
						messageIn, &aText, recursion_level + 1);
					if(rc == VIRUS) {
						infected = TRUE;
						break;
					}
				}

				/* rc = parseEmailBody(NULL, NULL, mctx, recursion_level + 1); */
				break;
			case SIGNED:
			case PARALLEL:
				/*
				 * If we're here it could be because we have a
				 * multipart/mixed message, consisting of a
				 * message followed by an attachment. That
				 * message itself is a multipart/alternative
				 * message and we need to dig out the plain
				 * text part of that alternative
				 */
				htmltextPart = getTextPart(messages, multiparts);
				if(htmltextPart == -1)
					htmltextPart = 0;

				rc = parseEmailBody(messages[htmltextPart], aText, mctx, recursion_level + 1);
				break;
			case ENCRYPTED:
				rc = FAIL;	/* Not yet handled */
				protocol = (char *)messageFindArgument(mainMessage, "protocol");
				if(protocol) {
					if(strcasecmp(protocol, "application/pgp-encrypted") == 0) {
						/* RFC2015 */
						cli_warnmsg("PGP encoded attachment not scanned\n");
						rc = OK_ATTACHMENTS_NOT_SAVED;
					} else
						cli_warnmsg("Unknown encryption protocol '%s' - if you believe this file contains a virus, submit it to www.clamav.net\n", protocol);
					free(protocol);
				} else
					cli_dbgmsg("Encryption method missing protocol name\n");

				break;
			default:
				assert(0);
			}

			if(mainMessage && (mainMessage != messageIn))
				messageDestroy(mainMessage);

			if(aText && (textIn == NULL)) {
				if((!infected) && (fb = fileblobCreate()) != NULL) {
					cli_dbgmsg("Save non mime and/or text/plain part\n");
					fileblobSetFilename(fb, mctx->dir, "textpart");
					/*fileblobAddData(fb, "Received: by clamd (textpart)\n", 30);*/
					fileblobSetCTX(fb, mctx->ctx);
					(void)textToFileblob(aText, fb, 1);

					fileblobDestroy(fb);
				}
				textDestroy(aText);
			}

			for(i = 0; i < multiparts; i++)
				if(messages[i])
					messageDestroy(messages[i]);

			if(messages)
				free(messages);

			return rc;

		case MESSAGE:
			/*
			 * Check for forbidden encodings
			 */
			switch(messageGetEncoding(mainMessage)) {
				case NOENCODING:
				case EIGHTBIT:
				case BINARY:
					break;
				default:
					cli_warnmsg("MIME type 'message' cannot be decoded\n");
					break;
			}
			rc = FAIL;
			if((strcasecmp(mimeSubtype, "rfc822") == 0) ||
			   (strcasecmp(mimeSubtype, "delivery-status") == 0)) {
				message *m = parseEmailHeaders(mainMessage, mctx->rfc821Table);
				if(m) {
					cli_dbgmsg("Decode rfc822\n");

					messageSetCTX(m, mctx->ctx);

					if(mainMessage && (mainMessage != messageIn)) {
						messageDestroy(mainMessage);
						mainMessage = NULL;
					} else
						messageReset(mainMessage);
					if(messageGetBody(m))
						rc = parseEmailBody(m, NULL, mctx, recursion_level + 1);

					messageDestroy(m);
				}
				break;
			} else if(strcasecmp(mimeSubtype, "disposition-notification") == 0) {
				/* RFC 2298 - handle like a normal email */
				rc = OK;
				break;
			} else if(strcasecmp(mimeSubtype, "partial") == 0) {
#ifdef	PARTIAL_DIR
				/* RFC1341 message split over many emails */
				if(rfc1341(mainMessage, mctx->dir) >= 0)
					rc = OK;
#else
				cli_warnmsg("Partial message received from MUA/MTA - message cannot be scanned\n");
#endif
			} else if(strcasecmp(mimeSubtype, "external-body") == 0)
				/* TODO */
				cli_warnmsg("Attempt to send Content-type message/external-body trapped");
			else
				cli_warnmsg("Unsupported message format `%s' - if you believe this file contains a virus, submit it to www.clamav.net\n", mimeSubtype);


			if(mainMessage && (mainMessage != messageIn))
				messageDestroy(mainMessage);
			if(messages)
				free(messages);
			return rc;

		case APPLICATION:
			/*cptr = messageGetMimeSubtype(mainMessage);

			if((strcasecmp(cptr, "octet-stream") == 0) ||
			   (strcasecmp(cptr, "x-msdownload") == 0)) {*/
			{
				fb = messageToFileblob(mainMessage, mctx->dir, 1);

				if(fb) {
					cli_dbgmsg("Saving main message as attachment\n");
					fileblobDestroy(fb);
					if(mainMessage != messageIn) {
						messageDestroy(mainMessage);
						mainMessage = NULL;
					} else
						messageReset(mainMessage);
				}
			} /*else
				cli_warnmsg("Discarded application not sent as attachment\n");*/
			break;

		case AUDIO:
		case VIDEO:
		case IMAGE:
			break;

		default:
			cli_warnmsg("Message received with unknown mime encoding");
			break;
		}

		if(messages) {
			/* "can't happen" */
			cli_warnmsg("messages != NULL, report to http://bugs.clamav.net\n");
			free(messages);
		}
	}

	if(aText && (textIn == NULL)) {
		/* Look for a bounce in the text (non mime encoded) portion */
		const text *t;

		for(t = aText; t; t = t->t_next) {
			const line_t *l = t->t_line;
			const text *lookahead, *topofbounce;
			const char *s;
			bool inheader;

			if(l == NULL)
				continue;

			if(!isBounceStart(lineGetData(l)))
				continue;

			/*
			 * We've found what looks like the start of a bounce
			 * message. Only bother saving if it really is a bounce
			 * message, this helps to speed up scanning of ping-pong
			 * messages that have lots of bounces within bounces in
			 * them
			 */
			for(lookahead = t->t_next; lookahead; lookahead = lookahead->t_next) {
				l = lookahead->t_line;

				if(l == NULL)
					break;
				s = lineGetData(l);
				if(strncasecmp(s, "Content-Type:", 13) == 0)
					/*
					 * Don't bother with plain/text or
					 * plain/html
					 */
					if(strstr(s, "text/") == NULL)
						/*
						 * Don't bother to save the unuseful
						 * part
						 */
						break;
			}

			if(lookahead && (lookahead->t_line == NULL)) {
				cli_dbgmsg("Non mime part bounce message is not mime encoded, so it will not be scanned\n");
				t = lookahead;
				/* look for next bounce message */
				continue;
			}

			/*
			 * Prescan the bounce message to see if there's likely
			 * to be anything nasty.
			 * This algorithm is hand crafted and may be breakable
			 * so all submissions are welcome. It's best NOT to
			 * remove this however you may be tempted, because it
			 * significantly speeds up the scanning of multiple
			 * bounces (i.e. bounces within many bounces)
			 */
			for(; lookahead; lookahead = lookahead->t_next) {
				l = lookahead->t_line;

				if(l) {
					s = lineGetData(l);
					if((strncasecmp(s, "Content-Type:", 13) == 0) &&
					   (strstr(s, "multipart/") == NULL) &&
					   (strstr(s, "message/rfc822") == NULL) &&
					   (strstr(s, "text/plain") == NULL))
						break;
				}
			}
			if(lookahead == NULL) {
				cli_dbgmsg("cli_mbox: I believe it's plain text which must be clean\n");
				/* nothing here, move along please */
				break;
			}
			if((fb = fileblobCreate()) == NULL)
				break;
			cli_dbgmsg("Save non mime part bounce message\n");
			fileblobSetFilename(fb, mctx->dir, "bounce");
			fileblobAddData(fb, (const unsigned char *)"Received: by clamd (bounce)\n", 28);
			fileblobSetCTX(fb, mctx->ctx);

			inheader = TRUE;
			topofbounce = NULL;
			do {
				l = t->t_line;

				if(l == NULL) {
					if(inheader) {
						inheader = FALSE;
						topofbounce = t;
					}
				} else {
					s = lineGetData(l);
					fileblobAddData(fb, (const unsigned char *)s, strlen(s));
				}
				fileblobAddData(fb, (const unsigned char *)"\n", 1);
				lookahead = t->t_next;
				if(lookahead == NULL)
					break;
				t = lookahead;
				l = t->t_line;
				if((!inheader) && l) {
					s = lineGetData(l);
					if(isBounceStart(s)) {
						cli_dbgmsg("Found the start of another bounce candidate (%s)\n", s);
						break;
					}
				}
			} while(!fileblobContainsVirus(fb));

			fileblobDestroy(fb);
			if(topofbounce)
				t = topofbounce;
			/*
			 * Don't do this - it slows bugs.txt
			 */
			/*if(mainMessage)
				mainMessage->bounce = NULL;*/
		}
		textDestroy(aText);
		aText = NULL;
	}

	/*
	 * No attachments - scan the text portions, often files
	 * are hidden in HTML code
	 */
	if(mainMessage && (rc != VIRUS)) {
		text *t_line;

		/*
		 * Look for uu-encoded main file
		 */
		if((encodingLine(mainMessage) != NULL) &&
		   ((t_line = bounceBegin(mainMessage)) != NULL)) {
			if(exportBounceMessage(t_line, mctx))
				rc = OK;
		} else {
			bool saveIt;

			if(messageGetMimeType(mainMessage) == MESSAGE)
				/*
				 * Quick peek, if the encapsulated
				 * message has no
				 * content encoding statement don't
				 * bother saving to scan, it's safe
				 */
				saveIt = (bool)(encodingLine(mainMessage) != NULL);
			else if((t_line = encodingLine(mainMessage)) != NULL) {
				/*
				 * Some bounces include the message
				 * body without the headers.
				 * FIXME: Unfortunately this generates a
				 * lot of false positives that a bounce
				 * has been found when it hasn't.
				 */
				if((fb = fileblobCreate()) != NULL) {
					cli_dbgmsg("Found a bounce message with no header at '%s'\n",
						lineGetData(t_line->t_line));
					fileblobSetFilename(fb, mctx->dir, "bounce");
					fileblobAddData(fb,
						(const unsigned char *)"Received: by clamd (bounce)\n",
						28);

					/*fileblobSetCTX(fb, ctx);*/
					fileblobDestroy(textToFileblob(t_line, fb, 1));
				}
				saveIt = FALSE;
			} else
				/*
				 * Save the entire text portion,
				 * since it it may be an HTML file with
				 * a JavaScript virus or a phish
				 */
				saveIt = TRUE;

			if(saveIt) {
				cli_dbgmsg("Saving text part to scan\n");
				saveTextPart(mainMessage, mctx->dir, 1);
				if(mainMessage != messageIn) {
					messageDestroy(mainMessage);
					mainMessage = NULL;
				} else
					messageReset(mainMessage);
				rc = OK;
			}
		}
	} else
		rc = OK_ATTACHMENTS_NOT_SAVED;	/* nothing saved */

	if(mainMessage && (mainMessage != messageIn))
		messageDestroy(mainMessage);

	if((rc != FAIL) && infected)
		rc = VIRUS;

	cli_dbgmsg("parseEmailBody() returning %d\n", rc);

	return rc;
}

/*
 * Is the current line the start of a new section?
 *
 * New sections start with --boundary
 */
static int
boundaryStart(const char *line, const char *boundary)
{
	const char *ptr;
	char *out;
	int rc;
	char buf[RFC2821LENGTH + 1];

	if(line == NULL)
		return 0;	/* empty line */
	if(boundary == NULL)
		return 0;

	/*cli_dbgmsg("boundaryStart: line = '%s' boundary = '%s'\n", line, boundary);*/

	if((*line != '-') && (*line != '('))
		return 0;

	if(strchr(line, '-') == NULL)
		return 0;

	if(strlen(line) <= sizeof(buf)) {
		out = NULL;
		ptr = rfc822comments(line, buf);
	} else
		ptr = out = rfc822comments(line, NULL);

	if(ptr == NULL)
		ptr = line;

	if(*ptr++ != '-') {
		if(out)
			free(out);
		return 0;
	}

	/*
	 * Gibe.B3 is broken, it has:
	 *	boundary="---- =_NextPart_000_01C31177.9DC7C000"
	 * but it's boundaries look like
	 *	------ =_NextPart_000_01C31177.9DC7C000
	 * notice the one too few '-'.
	 * Presumably this is a deliberate exploitation of a bug in some mail
	 * clients.
	 *
	 * The trouble is that this creates a lot of false positives for
	 * boundary conditions, if we're too lax about matches. We do our level
	 * best to avoid these false positives. For example if we have
	 * boundary="1" we want to ensure that we don't break out of every line
	 * that has -1 in it instead of starting --1. This needs some more work.
	 *
	 * Look with and without RFC822 comments stripped, I've seen some
	 * samples where () are taken as comments in boundaries and some where
	 * they're not. Irrespective of whatever RFC2822 says we need to find
	 * viruses in both types of mails
	 */
	if((strstr(ptr, boundary) != NULL) || (strstr(line, boundary) != NULL))
		rc = OK;
	else if(*ptr++ != '-')
		rc = FAIL;
	else
		rc = (strcasecmp(ptr, boundary) == 0);

	if(out)
		free(out);

	if(rc == OK)
		cli_dbgmsg("boundaryStart: found %s in %s\n", boundary, line);

	return rc;
}

/*
 * Is the current line the end?
 *
 * The message ends with with --boundary--
 */
static int
endOfMessage(const char *line, const char *boundary)
{
	size_t len;

	if(line == NULL)
		return 0;
	/*cli_dbgmsg("endOfMessage: line = '%s' boundary = '%s'\n", line, boundary);*/
	if(*line++ != '-')
		return 0;
	if(*line++ != '-')
		return 0;
	len = strlen(boundary);
	if(strncasecmp(line, boundary, len) != 0)
		return 0;
	/*
	 * Use < rather than == because some broken mails have white
	 * space after the boundary
	 */
	if(strlen(line) < (len + 2))
		return 0;
	line = &line[len];
	if(*line++ != '-')
		return 0;
	return *line == '-';
}

/*
 * Initialise the various lookup tables
 */
static int
initialiseTables(table_t **rfc821Table, table_t **subtypeTable)
{
	const struct tableinit *tableinit;

	/*
	 * Initialise the various look up tables
	 */
	*rfc821Table = tableCreate();
	assert(*rfc821Table != NULL);

	for(tableinit = rfc821headers; tableinit->key; tableinit++)
		if(tableInsert(*rfc821Table, tableinit->key, tableinit->value) < 0) {
			tableDestroy(*rfc821Table);
			*rfc821Table = NULL;
			return -1;
		}

	*subtypeTable = tableCreate();
	assert(*subtypeTable != NULL);

	for(tableinit = mimeSubtypes; tableinit->key; tableinit++)
		if(tableInsert(*subtypeTable, tableinit->key, tableinit->value) < 0) {
			tableDestroy(*rfc821Table);
			tableDestroy(*subtypeTable);
			*rfc821Table = NULL;
			*subtypeTable = NULL;
			return -1;
		}

	return 0;
}

/*
 * If there's a HTML text version use that, otherwise
 * use the first text part, otherwise just use the
 * first one around. HTML text is most likely to include
 * a scripting worm
 *
 * If we can't find one, return -1
 */
static int
getTextPart(message *const messages[], size_t size)
{
	size_t i;
	int textpart = -1;

	for(i = 0; i < size; i++) {
		assert(messages[i] != NULL);
		if(messageGetMimeType(messages[i]) == TEXT) {
			if(strcasecmp(messageGetMimeSubtype(messages[i]), "html") == 0)
				return (int)i;
			textpart = (int)i;
		}
	}
	return textpart;
}

/*
 * strip -
 *	Remove the trailing spaces from a buffer. Don't call this directly,
 * always call strstrip() which is a wrapper to this routine to be used with
 * NUL terminated strings. This code looks a bit strange because of it's
 * heritage from code that worked on strings that weren't necessarily NUL
 * terminated.
 * TODO: rewrite for clamAV
 *
 * Returns it's new length (a la strlen)
 *
 * len must be int not size_t because of the >= 0 test, it is sizeof(buf)
 *	not strlen(buf)
 */
static size_t
strip(char *buf, int len)
{
	register char *ptr;
	register size_t i;

	if((buf == NULL) || (len <= 0))
		return 0;

	i = strlen(buf);
	if(len > (int)(i + 1))
		return i;
	ptr = &buf[--len];

#if	defined(UNIX) || defined(C_LINUX) || defined(C_DARWIN)	/* watch - it may be in shared text area */
	do
		if(*ptr)
			*ptr = '\0';
	while((--len >= 0) && (!isgraph(*--ptr)) && (*ptr != '\n') && (*ptr != '\r'));
#else	/* more characters can be displayed on DOS */
	do
#ifndef	REAL_MODE_DOS
		if(*ptr)	/* C8.0 puts into a text area */
#endif
			*ptr = '\0';
	while((--len >= 0) && ((*--ptr == '\0') || (isspace((int)*ptr))));
#endif
	return((size_t)(len + 1));
}

/*
 * strstrip:
 *	Strip a given string
 */
size_t
strstrip(char *s)
{
	if(s == (char *)NULL)
		return(0);

	return(strip(s, (int)strlen(s) + 1));
}

static int
parseMimeHeader(message *m, const char *cmd, const table_t *rfc821Table, const char *arg)
{
	char *copy, *p;
	const char *ptr;
	int commandNumber;

	cli_dbgmsg("parseMimeHeader: cmd='%s', arg='%s'\n", cmd, arg);

	copy = rfc822comments(cmd, NULL);
	if(copy) {
		commandNumber = tableFind(rfc821Table, copy);
		free(copy);
	} else
		commandNumber = tableFind(rfc821Table, cmd);

	copy = rfc822comments(arg, NULL);

	if(copy)
		ptr = copy;
	else
		ptr = arg;

	switch(commandNumber) {
		case CONTENT_TYPE:
			/*
			 * Fix for non RFC1521 compliant mailers
			 * that send content-type: Text instead
			 * of content-type: Text/Plain, or
			 * just simply "Content-Type:"
			 */
			if(arg == NULL)
				/*
				 * According to section 4 of RFC1521:
				 * "Note also that a subtype specification is
				 * MANDATORY. There are no default subtypes"
				 *
				 * We have to break this and make an assumption
				 * for the subtype because virus writers and
				 * email client writers don't get it right
				 */
				 cli_warnmsg("Empty content-type received, no subtype specified, assuming text/plain; charset=us-ascii\n");
			else if(strchr(ptr, '/') == NULL)
				/*
				 * Empty field, such as
				 *	Content-Type:
				 * which I believe is illegal according to
				 * RFC1521
				 */
				cli_dbgmsg("Invalid content-type '%s' received, no subtype specified, assuming text/plain; charset=us-ascii\n", ptr);
			else {
				int i;
				char *mimeArgs;	/* RHS of the ; */

				/*
				 * Some clients are broken and
				 * put white space after the ;
				 */
				if(*arg == '/') {
					cli_warnmsg("Content-type '/' received, assuming application/octet-stream\n");
					messageSetMimeType(m, "application");
					messageSetMimeSubtype(m, "octet-stream");
				} else {
					/*
					 * The content type could be in quotes:
					 *	Content-Type: "multipart/mixed"
					 * FIXME: this is a hack in that ignores
					 *	the quotes, it doesn't handle
					 *	them properly
					 */
					while(isspace(*ptr))
						ptr++;
					if(ptr[0] == '\"')
						ptr++;

					if(ptr[0] != '/') {
						char *s;
						char *mimeType;	/* LHS of the ; */
#ifdef CL_THREAD_SAFE
						char *strptr = NULL;
#endif

						s = mimeType = cli_strtok(ptr, 0, ";");
						/*
						 * Handle
						 * Content-Type: foo/bar multipart/mixed
						 * and
						 * Content-Type: multipart/mixed foo/bar
						 */
						if(s && *s) for(;;) {
#ifdef	CL_THREAD_SAFE
							int set = messageSetMimeType(m, strtok_r(s, "/", &strptr));
#else
							int set = messageSetMimeType(m, strtok(s, "/"));
#endif

							/*
							 * Stephen White <stephen@earth.li>
							 * Some clients put space after
							 * the mime type but before
							 * the ;
							 */
#ifdef	CL_THREAD_SAFE
							s = strtok_r(NULL, ";", &strptr);
#else
							s = strtok(NULL, ";");
#endif
							if(s == NULL)
								break;
							if(set) {
								size_t len = strstrip(s) - 1;
								if(s[len] == '\"') {
									s[len] = '\0';
									len = strstrip(s);
								}
								if(len) {
									if(strchr(s, ' ')) {
										char *t = cli_strtok(s, 0, " ");

										messageSetMimeSubtype(m, t);
										free(t);
									} else
										messageSetMimeSubtype(m, s);
								}
							}

							while(*s && !isspace(*s))
								s++;
							if(*s++ == '\0')
								break;
							if(*s == '\0')
								break;
						}
						if(mimeType)
							free(mimeType);
					}
				}

				/*
				 * Add in all rest of the the arguments.
				 * e.g. if the header is this:
				 * Content-Type:', arg='multipart/mixed; boundary=foo
				 * we find the boundary argument set it
				 */
				i = 1;
				while((mimeArgs = cli_strtok(ptr, i++, ";")) != NULL) {
					cli_dbgmsg("mimeArgs = '%s'\n", mimeArgs);

					messageAddArguments(m, mimeArgs);
					free(mimeArgs);
				}
			}
			break;
		case CONTENT_TRANSFER_ENCODING:
			messageSetEncoding(m, ptr);
			break;
		case CONTENT_DISPOSITION:
			p = cli_strtok(ptr, 0, ";");
			if(p) {
				if(*p) {
					messageSetDispositionType(m, p);
					free(p);
					p = cli_strtok(ptr, 1, ";");
					messageAddArgument(m, p);
				}
				free(p);
			}
			if((p = (char *)messageFindArgument(m, "filename")) == NULL)
				/*
				 * Handle this type of header, without
				 * a filename (e.g. some Worm.Torvil.D)
				 *	Content-ID: <nRfkHdrKsAxRU>
				 * Content-Transfer-Encoding: base64
				 * Content-Disposition: attachment
				 */
				messageAddArgument(m, "filename=unknown");
			else
				free(p);
	}
	if(copy)
		free(copy);

	return 0;
}

/*
 * Save the text portion of the message
 */
static void
saveTextPart(message *m, const char *dir, int destroy_text)
{
	fileblob *fb;

	messageAddArgument(m, "filename=textportion");
	if((fb = messageToFileblob(m, dir, destroy_text)) != NULL) {
		/*
		 * Save main part to scan that
		 */
		cli_dbgmsg("Saving main message\n");

		fileblobDestroy(fb);
	}
}

/*
 * Handle RFC822 comments in headers.
 * If out == NULL, return a buffer without the comments, the caller must free
 *	the returned buffer
 * Return NULL on error or if the input * has no comments.
 * See secion 3.4.3 of RFC822
 * TODO: handle comments that go on to more than one line
 */
static char *
rfc822comments(const char *in, char *out)
{
	const char *iptr;
	char *optr;
	int backslash, inquote, commentlevel;

	if(in == NULL)
		return NULL;

	if(strchr(in, '(') == NULL)
		return NULL;

	assert(out != in);

	if(out == NULL) {
		out = cli_malloc(strlen(in) + 1);
		if(out == NULL)
			return NULL;
	}

	backslash = commentlevel = inquote = 0;
	optr = out;

	cli_dbgmsg("rfc822comments: contains a comment\n");

	for(iptr = in; *iptr; iptr++)
		if(backslash) {
			if(commentlevel == 0)
				*optr++ = *iptr;
			backslash = 0;
		} else switch(*iptr) {
			case '\\':
				backslash = 1;
				break;
			case '\"':
				*optr++ = '\"';
				inquote = !inquote;
				break;
			case '(':
				if(inquote)
					*optr++ = '(';
				else
					commentlevel++;
				break;
			case ')':
				if(inquote)
					*optr++ = ')';
				else if(commentlevel > 0)
					commentlevel--;
				break;
			default:
				if(commentlevel == 0)
					*optr++ = *iptr;
		}

	if(backslash)	/* last character was a single backslash */
		*optr++ = '\\';
	*optr = '\0';

	/*strstrip(out);*/

	cli_dbgmsg("rfc822comments '%s'=>'%s'\n", in, out);

	return out;
}

/*
 * Handle RFC2047 encoding. Returns a malloc'd buffer that the caller must
 * free, or NULL on error
 */
static char *
rfc2047(const char *in)
{
	char *out, *pout;
	size_t len;

	if((strstr(in, "=?") == NULL) || (strstr(in, "?=") == NULL))
		return cli_strdup(in);

	cli_dbgmsg("rfc2047 '%s'\n", in);
	out = cli_malloc(strlen(in) + 1);

	if(out == NULL)
		return NULL;

	pout = out;

	/* For each RFC2047 string */
	while(*in) {
		char encoding, *ptr, *enctext;
		message *m;
		blob *b;

		/* Find next RFC2047 string */
		while(*in) {
			if((*in == '=') && (in[1] == '?')) {
				in += 2;
				break;
			}
			*pout++ = *in++;
		}
		/* Skip over charset, find encoding */
		while((*in != '?') && *in)
			in++;
		if(*in == '\0')
			break;
		encoding = *++in;
		encoding = tolower(encoding);

		if((encoding != 'q') && (encoding != 'b')) {
			cli_warnmsg("Unsupported RFC2047 encoding type '%c' - if you believe this file contains a virus, submit it to www.clamav.net\n", encoding);
			free(out);
			out = NULL;
			break;
		}
		/* Skip to encoded text */
		if(*++in != '?')
			break;
		if(*++in == '\0')
			break;

		enctext = cli_strdup(in);
		if(enctext == NULL) {
			free(out);
			out = NULL;
			break;
		}
		in = strstr(in, "?=");
		if(in == NULL) {
			free(enctext);
			break;
		}
		in += 2;
		ptr = strstr(enctext, "?=");
		assert(ptr != NULL);
		*ptr = '\0';
		/*cli_dbgmsg("Need to decode '%s' with method '%c'\n", enctext, encoding);*/

		m = messageCreate();
		if(m == NULL)
			break;
		messageAddStr(m, enctext);
		free(enctext);
		switch(encoding) {
			case 'q':
				messageSetEncoding(m, "quoted-printable");
				break;
			case 'b':
				messageSetEncoding(m, "base64");
				break;
		}
		b = messageToBlob(m, 1);
                if (b == NULL) {
                    messageDestroy(m);
                    break;
                }
		len = blobGetDataSize(b);
		cli_dbgmsg("Decoded as '%*.*s'\n", (int)len, (int)len,
			blobGetData(b));
		memcpy(pout, blobGetData(b), len);
		blobDestroy(b);
		messageDestroy(m);
		if(len > 0 && pout[len - 1] == '\n')
			pout += len - 1;
		else
			pout += len;

	}
	if(out == NULL)
		return NULL;

	*pout = '\0';

	cli_dbgmsg("rfc2047 returns '%s'\n", out);
	return out;
}

#ifdef	PARTIAL_DIR
/*
 * Handle partial messages
 */
static int
rfc1341(message *m, const char *dir)
{
	fileblob *fb;
	char *arg, *id, *number, *total, *oldfilename;
	const char *tmpdir;
	char pdir[NAME_MAX + 1];

	id = (char *)messageFindArgument(m, "id");
	if(id == NULL)
		return -1;

#ifdef  C_CYGWIN
	if((tmpdir = getenv("TEMP")) == (char *)NULL)
		if((tmpdir = getenv("TMP")) == (char *)NULL)
			if((tmpdir = getenv("TMPDIR")) == (char *)NULL)
				tmpdir = "C:\\";
#else
	if((tmpdir = getenv("TMPDIR")) == (char *)NULL)
		if((tmpdir = getenv("TMP")) == (char *)NULL)
			if((tmpdir = getenv("TEMP")) == (char *)NULL)
#ifdef	P_tmpdir
				tmpdir = P_tmpdir;
#else
				tmpdir = "/tmp";
#endif
#endif

	snprintf(pdir, sizeof(pdir) - 1, "%s/clamav-partial", tmpdir);

	if((mkdir(pdir, 0700) < 0) && (errno != EEXIST)) {
		cli_errmsg("Can't create the directory '%s'\n", pdir);
		return -1;
	} else {
		struct stat statb;

		if(stat(pdir, &statb) < 0) {
			cli_errmsg("Can't stat the directory '%s'\n", pdir);
			return -1;
		}
		if(statb.st_mode & 077)
			cli_warnmsg("Insecure partial directory %s (mode 0%o)\n",
				pdir, (int)(statb.st_mode & 0777));
	}

	number = (char *)messageFindArgument(m, "number");
	if(number == NULL) {
		free(id);
		return -1;
	}

	oldfilename = (char *)messageFindArgument(m, "filename");
	if(oldfilename == NULL)
		oldfilename = (char *)messageFindArgument(m, "name");

	arg = cli_malloc(10 + strlen(id) + strlen(number));
	if(arg) {
		sprintf(arg, "filename=%s%s", id, number);
		messageAddArgument(m, arg);
		free(arg);
	}

	if(oldfilename) {
		cli_warnmsg("Must reset to %s\n", oldfilename);
		free(oldfilename);
	}

	if((fb = messageToFileblob(m, pdir, 0)) == NULL) {
		free(id);
		free(number);
		return -1;
	}

	fileblobDestroy(fb);

	total = (char *)messageFindArgument(m, "total");
	cli_dbgmsg("rfc1341: %s, %s of %s\n", id, number, (total) ? total : "?");
	if(total) {
		int n = atoi(number);
		int t = atoi(total);
		DIR *dd = NULL;

		free(total);
		/*
		 * If it's the last one - reassemble it
		 * FIXME: this assumes that we receive the parts in order
		 */
		if((n == t) && ((dd = opendir(pdir)) != NULL)) {
			FILE *fout;
			char outname[NAME_MAX + 1];
			time_t now;

			sanitiseName(id);

			snprintf(outname, sizeof(outname) - 1, "%s/%s", dir, id);

			cli_dbgmsg("outname: %s\n", outname);

			fout = fopen(outname, "wb");
			if(fout == NULL) {
				cli_errmsg("Can't open '%s' for writing", outname);
				free(id);
				free(number);
				closedir(dd);
				return -1;
			}

			time(&now);
			for(n = 1; n <= t; n++) {
				char filename[NAME_MAX + 1];
				const struct dirent *dent;
#if defined(HAVE_READDIR_R_3) || defined(HAVE_READDIR_R_2)
				union {
					struct dirent d;
					char b[offsetof(struct dirent, d_name) + NAME_MAX + 1];
				} result;
#endif

				snprintf(filename, sizeof(filename), "%s%d", id, n);

#ifdef HAVE_READDIR_R_3
				while((readdir_r(dd, &result.d, &dent) == 0) && dent) {
#elif defined(HAVE_READDIR_R_2)
				while((dent = (struct dirent *)readdir_r(dd, &result.d))) {
#else	/*!HAVE_READDIR_R*/
				while((dent = readdir(dd))) {
#endif
					FILE *fin;
					char buffer[BUFSIZ], fullname[NAME_MAX + 1];
					int nblanks;
					struct stat statb;
					extern short cli_leavetemps_flag;

#ifndef  C_CYGWIN
					if(dent->d_ino == 0)
						continue;
#endif

					snprintf(fullname, sizeof(fullname) - 1,
						"%s/%s", pdir, dent->d_name);

					if(strncmp(filename, dent->d_name, strlen(filename)) != 0) {
						if(!cli_leavetemps_flag)
							continue;
						if(stat(fullname, &statb) < 0)
							continue;
						if(now - statb.st_mtime > (time_t)(7 * 24 * 3600))
							if(unlink(fullname) >= 0)
								cli_warnmsg("removed old RFC1341 file %s\n", fullname);
						continue;
					}

					fin = fopen(fullname, "rb");
					if(fin == NULL) {
						cli_errmsg("Can't open '%s' for reading", fullname);
						fclose(fout);
						unlink(outname);
						free(id);
						free(number);
						closedir(dd);
						return -1;
					}
					nblanks = 0;
					while(fgets(buffer, sizeof(buffer) - 1, fin) != NULL)
						/*
						 * Ensure that trailing newlines
						 * aren't copied
						 */
						if(buffer[0] == '\n')
							nblanks++;
						else {
							if(nblanks)
								do
									putc('\n', fout);
								while(--nblanks > 0);
							fputs(buffer, fout);
						}
					fclose(fin);

					/* don't unlink if leave temps */
					if(!cli_leavetemps_flag)
						unlink(fullname);
					break;
				}
				rewinddir(dd);
			}
			closedir(dd);
			fclose(fout);
		}
	}
	free(number);
	free(id);

	return 0;
}
#endif

#ifdef CL_EXPERIMENTAL
static void
hrefs_done(blob *b, tag_arguments_t *hrefs)
{
	if(b)
		blobDestroy(b);
	html_tag_arg_free(hrefs);
}

/*
 * This used to be part of checkURLs, split out, because phishingScan needs it
 * too, and phishingScan might be used in situations where checkURLs is
 * disabled (see ifdef)
 */
static blob *
getHrefs(message *m, tag_arguments_t *hrefs)
{
	blob *b = messageToBlob(m, 0);
	size_t len;

	if(b == NULL)
		return NULL;

	len = blobGetDataSize(b);

	if(len == 0) {
		blobDestroy(b);
		return NULL;
	}

	/* TODO: make this size customisable */
	if(len > 100*1024) {
		cli_warnmsg("Viruses pointed to by URLs not scanned in large message\n");
		blobDestroy(b);
		return NULL;
	}

	hrefs->count = 0;
	hrefs->tag = hrefs->value = NULL;
	hrefs->contents = NULL;

	cli_dbgmsg("getHrefs: calling html_normalise_mem\n");
	if(!html_normalise_mem(blobGetData(b), (off_t)len, NULL, hrefs)) {
		blobDestroy(b);
		return NULL;
	}
	cli_dbgmsg("getHrefs: html_normalise_mem returned\n");

	/* TODO: Do we need to call remove_html_comments? */
	return b;
}

static void
checkURLs(message *mainMessage, mbox_ctx *mctx, mbox_status *rc, int is_html)
{
	tag_arguments_t hrefs;
	blob *b;

	/* aCaB: stripped GA related stuff */
	hrefs.scanContents = mctx->ctx->engine->dboptions&CL_DB_PHISHING_URLS;

#if    (!defined(FOLLOWURLS)) || (FOLLOWURLS <= 0)
	if(!hrefs.scanContents)
		/*
		 * Don't waste time extracting hrefs (parsing html), nobody
		 * will need it
		 */
		return;
#endif

	hrefs.count = 0;
	hrefs.tag = hrefs.value = NULL;
	hrefs.contents = NULL;

	b = getHrefs(mainMessage, &hrefs);
	if(b) {
		if(hrefs.scanContents /*mctx->ctx->engine->dboptions&CL_DB_PHISHING_URLS*/) {
			if(phishingScan(mainMessage, mctx->dir, mctx->ctx, &hrefs) == CL_VIRUS) {
				mainMessage->isInfected = TRUE;
				*rc = VIRUS;
				cli_dbgmsg("PH:Phishing found\n");
			}
		}
		if(is_html && (mctx->ctx->options&CL_SCAN_MAILURL) && (*rc != VIRUS))
			do_checkURLs(mainMessage, mctx->dir, &hrefs);
	}
	hrefs_done(b,&hrefs);
}

#if	defined(FOLLOWURLS) && (FOLLOWURLS > 0)
static void
do_checkURLs(message *m, const char *dir, tag_arguments_t *hrefs)
{
	table_t *t;
	int i, n;
#ifdef	CL_THREAD_SAFE
	pthread_t tid[FOLLOWURLS];
	struct arg args[FOLLOWURLS];
#endif

	t = tableCreate();
	if(t == NULL)
		return;

	n = 0;

	for(i = 0; i < hrefs->count; i++) {
		const char *url = (const char *)hrefs->value[i];

		/*
		 * TODO: If it's an image source, it'd be nice to note beacons
		 *	where width="0" height="0", which needs support from
		 *	the HTML normalise code
		 */
		if(strncasecmp("http://", url, 7) == 0) {
			char *ptr;
#ifndef	CL_THREAD_SAFE
			struct arg arg;
#endif
			char name[NAME_MAX + 1];

			if(tableFind(t, url) == 1) {
				cli_dbgmsg("URL %s already downloaded\n", url);
				continue;
			}
			/*
			 * What about foreign character spoofing?
			 * It would be useful be able to check if url
			 *	is the same as the text displayed, e.g.
			 *	<a href="http://dodgy.biz">www.paypal.com</a>
			 *	but that needs support from HTML normalise
			 */
			if(strchr(url, '%') && strchr(url, '@'))
				cli_warnmsg("Possible URL spoofing attempt noticed, but not yet handled (%s)\n", url);

			if(n == FOLLOWURLS) {
				cli_warnmsg("URL %s will not be scanned\n", url);
				break;
			}

			(void)tableInsert(t, url, 1);
			cli_dbgmsg("Downloading URL %s to be scanned\n", url);
			strncpy(name, url, sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
			for(ptr = name; *ptr; ptr++)
				if(*ptr == '/')
					*ptr = '_';

#ifdef	CL_THREAD_SAFE
			args[n].dir = dir;
			args[n].url = cli_strdup(url);
			args[n].filename = cli_strdup(name);
			pthread_create(&tid[n], NULL, getURL, &args[n]);
#else
			arg.url = cli_strdup(url);
			arg.dir = dir;
			arg.filename = name;
			getURL(&arg);
			free(arg.url);
#endif
			++n;
		}
	}
	tableDestroy(t);

#ifdef	CL_THREAD_SAFE
	assert(n <= FOLLOWURLS);
	cli_dbgmsg("checkURLs: waiting for %d thread(s) to finish\n", n);
	while(--n >= 0) {
		pthread_join(tid[n], NULL);
		free(args[n].filename);
		free(args[n].url);
	}
#endif
}
#else
static void
do_checkURLs(message *m, const char *dir, tag_arguments_t *hrefs)
{
}
#endif

#else	/*!CL_EXPERIMENTAL*/

#if	defined(FOLLOWURLS) && (FOLLOWURLS > 0)
static void
checkURLs(message *m, mbox_ctx *mctx, mbox_status *rc, int is_html)
{
	blob *b = messageToBlob(m, 0);
	size_t len;
	table_t *t;
	int i, n;
#if	defined(WITH_CURL) && defined(CL_THREAD_SAFE)
	pthread_t tid[FOLLOWURLS];
	struct arg args[FOLLOWURLS];
#endif
	tag_arguments_t hrefs;

	if(b == NULL)
		return;

	len = blobGetDataSize(b);

	if(len == 0) {
		blobDestroy(b);
		return;
	}

	/* TODO: make this size customisable */
	if(len > 100*1024) {
		cli_warnmsg("Viruses pointed to by URL not scanned in large message\n");
		blobDestroy(b);
		return;
	}

	t = tableCreate();
	if(t == NULL) {
		blobDestroy(b);
		return;
	}

	hrefs.count = 0;
	hrefs.tag = hrefs.value = NULL;

	cli_dbgmsg("checkURLs: calling html_normalise_mem\n");
	if(!html_normalise_mem(blobGetData(b), len, NULL, &hrefs)) {
		blobDestroy(b);
		tableDestroy(t);
		return;
	}
	cli_dbgmsg("checkURLs: html_normalise_mem returned\n");

	/* TODO: Do we need to call remove_html_comments? */

	n = 0;

	for(i = 0; i < hrefs.count; i++) {
		const char *url = (const char *)hrefs.value[i];

		/*
		 * TODO: If it's an image source, it'd be nice to note beacons
		 *	where width="0" height="0", which needs support from
		 *	the HTML normalise code
		 */
		if(strncasecmp("http://", url, 7) == 0) {
			char *ptr;
#ifdef	WITH_CURL
#ifndef	CL_THREAD_SAFE
			struct arg arg;
#endif

#else	/*!WITH_CURL*/
#ifdef	CL_THREAD_SAFE
			static pthread_mutex_t system_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
			struct stat statb;
			char cmd[512];
#endif	/*WITH_CURL*/
			char name[NAME_MAX + 1];

			if(tableFind(t, url) == 1) {
				cli_dbgmsg("URL %s already downloaded\n", url);
				continue;
			}
			/*
			 * What about foreign character spoofing?
			 * It would be useful be able to check if url
			 *	is the same as the text displayed, e.g.
			 *	<a href="http://dodgy.biz">www.paypal.com</a>
			 *	but that needs support from HTML normalise
			 */
			if(strchr(url, '%') && strchr(url, '@'))
				cli_warnmsg("Possible URL spoofing attempt noticed, but not yet handled (%s)\n", url);

			if(n == FOLLOWURLS) {
				cli_warnmsg("URL %s will not be scanned\n", url);
				break;
			}

			(void)tableInsert(t, url, 1);
			cli_dbgmsg("Downloading URL %s to be scanned\n", url);
			strncpy(name, url, sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
			for(ptr = name; *ptr; ptr++)
				if(*ptr == '/')
					*ptr = '_';

#ifdef	WITH_CURL
#ifdef	CL_THREAD_SAFE
			args[n].curl = curl_easy_init();
			if(args[n].curl == NULL) {
				cli_errmsg("curl_easy_init failed\n");
				continue;
			}
			args[n].dir = mctx->dir;
			args[n].url = url;
			args[n].filename = cli_strdup(name);
			pthread_create(&tid[n], NULL, getURL, &args[n]);
#else
			/* easy isn't the word I'd use... */
			arg.curl = curl_easy_init();
			if(arg.curl == NULL) {
				cli_errmsg("curl_easy_init failed\n");
				continue;
			}
			arg.url = url;
			arg.dir = mctx->dir;
			arg.filename = name;
			getURL(&arg);
			curl_easy_cleanup(arg.curl);
#endif

#else	/*!WITH_CURL*/
			cli_warnmsg("The use of mail-follow-urls without CURL being installed is deprecated\n");
			/*
			 * TODO: maximum size and timeouts
			 */
			len = sizeof(cmd) - 26 - strlen(mctx->dir) - strlen(name);
#ifdef	CL_DEBUG
			snprintf(cmd, sizeof(cmd) - 1, "GET -t10 \"%.*s\" >%s/%s", len, url, mctx->dir, name);
#else
			snprintf(cmd, sizeof(cmd) - 1, "GET -t10 \"%.*s\" >%s/%s 2>/dev/null", len, url, mctx->dir, name);
#endif
			cmd[sizeof(cmd) - 1] = '\0';

			cli_dbgmsg("%s\n", cmd);
#ifdef	CL_THREAD_SAFE
			pthread_mutex_lock(&system_mutex);
#endif
			system(cmd);
#ifdef	CL_THREAD_SAFE
			pthread_mutex_unlock(&system_mutex);
#endif
			snprintf(cmd, sizeof(cmd), "%s/%s", mctx->dir, name);
			if(stat(cmd, &statb) >= 0)
				if(statb.st_size == 0) {
					cli_warnmsg("URL %s failed to download\n", url);
					/*
					 * Don't bother scanning an empty file
					 */
					(void)unlink(cmd);
				}
#endif
			++n;
		}
	}
	blobDestroy(b);
	tableDestroy(t);

#if	defined(WITH_CURL) && defined(CL_THREAD_SAFE)
	assert(n <= FOLLOWURLS);
	cli_dbgmsg("checkURLs: waiting for %d thread(s) to finish\n", n);
	while(--n >= 0) {
		pthread_join(tid[n], NULL);
		free(args[n].filename);
		curl_easy_cleanup(args[n].curl);
	}
#endif
	html_tag_arg_free(&hrefs);
}

#else

static void
checkURLs(message *m, mbox_ctx *mctx, mbox_status* rc, int is_html)
{
}
#endif
#endif /* CL_EXPERIMENTAL */

#if	defined(FOLLOWURLS) && (FOLLOWURLS > 0)
/*
 * Includes some Win32 patches by Gianluigi Tiesi <sherpya@netfarm.it>
 *
 * FIXME: Often WMF exploits work by sending people an email directing them
 *	to a page which displays a picture containing the exploit. This is not
 *	currently found, since only the HTML on the referred page is downloaded.
 *	It would be useful to scan the HTML for references to pictures and
 *	download them for scanning. But that will hit performance so there is
 *	an issue here.
 */

#if	defined(CL_EXPERIMENTAL) || (!defined(WITH_CURL))

/*
 * Removing the reliance on libcurl
 * Includes some of the freshclam hacks by Everton da Silva Marques
 * everton.marques@gmail.com>
 */
#ifndef timercmp
# define timercmp(a, b, cmp)	  \
  (((a)->tv_sec == (b)->tv_sec) ?	\
   ((a)->tv_usec cmp (b)->tv_usec) :  \
   ((a)->tv_sec cmp (b)->tv_sec))
#endif /* timercmp */

#ifndef timersub
# define timersub(a, b, result)	 \
  do {				\
	(result)->tv_sec = (a)->tv_sec - (b)->tv_sec;	\
	(result)->tv_usec = (a)->tv_usec - (b)->tv_usec;  \
	if ((result)->tv_usec < 0) {			\
		--(result)->tv_sec;			 \
		(result)->tv_usec += 1000000;		 \
	}						 \
  } while (0)
#endif /* timersub */

static	long	nonblock_fcntl(int sock);
static	void	restore_fcntl(int sock, long fcntl_flags);
static	int	nonblock_connect(int sock, const struct sockaddr *addr, socklen_t addrlen, int secs);
static	int	connect_error(int sock);
static	int	my_r_gethostbyname(const char *hostname, struct hostent *hp, char *buf, size_t len);

#define NONBLOCK_SELECT_MAX_FAILURES	3
#define NONBLOCK_MAX_BOGUS_LOOPS	10

static void *
#ifdef	CL_THREAD_SAFE
getURL(void *a)
#else
getURL(struct arg *arg)
#endif
{
	FILE *fp;
#ifdef	CL_THREAD_SAFE
	struct arg *arg = (struct arg *)a;
#endif
	const char *url = arg->url;
	const char *dir = arg->dir;
	const char *filename = arg->filename;
	char fout[NAME_MAX + 1];
#ifdef	C_WINDOWS
	SOCKET sd;
#else
	int sd;
#endif
	int n;
	struct sockaddr_in server;
#ifdef	HAVE_IN_ADDR_T
	in_addr_t ip;
#else
	unsigned int ip;
#endif
	char buf[BUFSIZ];
	char site[BUFSIZ];
	in_port_t port;
	static in_port_t default_port;
	static int tcp;
	int doingsite, firstpacket;
	char *ptr;
	int flags, via_proxy;
	const char *proxy;

	if(strlen(url) > (sizeof(site) - 1)) {
		cli_dbgmsg("Ignoring long URL \"%s\"\n", url);
		return NULL;
	}

	snprintf(fout, sizeof(fout) - 1, "%s/%s", dir, filename);

	fp = fopen(fout, "wb");

	if(fp == NULL) {
		cli_errmsg("Can't open '%s' for writing", fout);
		return NULL;
	}
	cli_dbgmsg("Saving %s to %s\n", url, fout);

#ifndef	C_BEOS
	if(tcp == 0) {
		const struct protoent *proto = getprotobyname("tcp");

		if(proto == NULL) {
			cli_warnmsg("Unknown prototol tcp, check /etc/protocols\n");
			fclose(fp);
			return NULL;
		}
		tcp = proto->p_proto;
#ifndef	C_WINDOWS
		endprotoent();
#endif
	}
#endif
	if(default_port == 0) {
		const struct servent *servent = getservbyname("http", "tcp");

		if(servent)
			default_port = (in_port_t)ntohs(servent->s_port);
		else
			default_port = 80;
#if	!defined(C_WINDOWS) && !defined(C_BEOS)
		endservent();
#endif
	}
	port = default_port;

	doingsite = 1;
	ptr = site;

	proxy = getenv("http_proxy");	/* FIXME: handle no_proxy */

	via_proxy = (proxy && *proxy);

	if(via_proxy) {
		if(strncasecmp(proxy, "http://", 7) != 0) {
			cli_warnmsg("Unsupported proxy protocol\n");
			fclose(fp);
			return NULL;
		}

		cli_dbgmsg("Getting %s via %s\n", url, proxy);

		proxy += 7;
		while(*proxy) {
			if(doingsite && (*proxy == ':')) {
				port = 0;
				while(isdigit(*++proxy)) {
					port *= 10;
					port += *proxy - '0';
				}
				continue;
			}
			if(doingsite && (*proxy == '/')) {
				proxy++;
				break;
			}
			*ptr++ = *proxy++;
		}
	} else {
		cli_dbgmsg("Getting %s\n", url);

		if(strncasecmp(url, "http://", 7) != 0) {
			cli_warnmsg("Unsupported protocol\n");
			fclose(fp);
			return NULL;
		}

		url += 7;
		while(*url) {
			if(doingsite && (*url == ':')) {
				port = 0;
				while(isdigit(*++url)) {
					port *= 10;
					port += *url - '0';
				}
				continue;
			}
			if(doingsite && (*url == '/')) {
				url++;
				break;
			}
			*ptr++ = *url++;
		}
	}
	*ptr = '\0';

	memset((char *)&server, '\0', sizeof(struct sockaddr_in));
	server.sin_family = AF_INET;
	server.sin_port = (in_port_t)htons(port);

	ip = inet_addr(site);
#ifdef	INADDR_NONE
	if(ip == INADDR_NONE) {
#else
	if(ip == (in_addr_t)-1) {
#endif
		struct hostent h;

		if((my_r_gethostbyname(site, &h, buf, sizeof(buf)) != 0) ||
		   (h.h_addr_list == NULL) ||
		   (h.h_addr == NULL)) {
			cli_dbgmsg("Unknown host %s\n", site);
			fclose(fp);
			return NULL;
		}

		memcpy((char *)&ip, h.h_addr, sizeof(ip));
	}
	server.sin_addr.s_addr = ip;
	if((sd = socket(AF_INET, SOCK_STREAM, tcp)) < 0) {
		fclose(fp);
		return NULL;
	}
	flags = nonblock_fcntl(sd);
	if(nonblock_connect(sd, (struct sockaddr *)&server, sizeof(struct sockaddr_in), 5) < 0) {
		closesocket(sd);
		fclose(fp);
		return NULL;
	}

	restore_fcntl(sd, flags);
	/*
	 * TODO: consider HTTP/1.1
	 */
	if(via_proxy)
		snprintf(buf, sizeof(buf) - 1,
			"GET %s HTTP/1.0\nUser-Agent: www.clamav.net\n\n", url);
	else
		snprintf(buf, sizeof(buf) - 1,
			"GET /%s HTTP/1.0\nUser-Agent: www.clamav.net\n\n", url);

	/*cli_dbgmsg("%s", buf);*/

	if(send(sd, buf, (int)strlen(buf), 0) < 0) {
		closesocket(sd);
		fclose(fp);
		return NULL;
	}

#ifdef	SHUT_WR
	shutdown(sd, SHUT_WR);
#else
	shutdown(sd, 1);
#endif

	firstpacket = 1;

	for(;;) {
		fd_set set;
		struct timeval tv;

		FD_ZERO(&set);
		FD_SET(sd, &set);

		tv.tv_sec = 30;	/* FIXME: make this customisable */
		tv.tv_usec = 0;

		if(select(sd + 1, &set, NULL, NULL, &tv) < 0) {
			if(errno == EINTR)
				continue;
			closesocket(sd);
			fclose(fp);
			return NULL;
		}
		if(!FD_ISSET(sd, &set)) {
			fclose(fp);
			closesocket(sd);
			return NULL;
		}
		n = recv(sd, buf, BUFSIZ, 0);

		if(n < 0) {
			fclose(fp);
			closesocket(sd);
			return NULL;
		}
		if(n == 0)
			break;

		/*
		 * FIXME: Handle header in more than one packet
		 */
		if(firstpacket) {
			char *statusptr;

			buf[n] = '\0';

			statusptr = cli_strtok(buf, 1, " ");

			if(statusptr) {
				int status = atoi(statusptr);

				cli_dbgmsg("HTTP status %d\n", status);

				free(statusptr);

				if((status == 301) || (status == 302)) {
					char *location;

					location = strstr(buf, "\nLocation: ");

					if(location) {
						char *end;

						fclose(fp);
						closesocket(sd);
						unlink(fout);

						location += 11;
						free(arg->url);
						end = location;
						while(*end && (*end != '\n'))
							end++;
						*end = '\0';
						arg->url = cli_strdup(location);
						cli_dbgmsg("Redirecting to %s\n", arg->url);
						return getURL(arg);
					}
				}
			}
			/*
			 * Don't write the HTTP header
			 */
			ptr = strstr(buf, "\n\n");
			if(ptr != NULL) {
				ptr += 2;
				n -= (int)(ptr - buf);
			} else
				ptr = buf;

			firstpacket = 0;
		} else
			ptr = buf;

		if(fwrite(ptr, n, 1, fp) != 1) {
			cli_warnmsg("Error writing %d bytes to %s\n",
				n, fout);
			break;
		}
	}

	fclose(fp);
	closesocket(sd);
	return NULL;
}

/*
 * Have a copy here because r_gethostbyname is in shared not libclamav :-(
 */
static int
my_r_gethostbyname(const char *hostname, struct hostent *hp, char *buf, size_t len)
{
#if	defined(HAVE_GETHOSTBYNAME_R_6)
	/* e.g. Linux */
	struct hostent *hp2;
	int ret = -1;

	if((hostname == NULL) || (hp == NULL))
		return -1;
	if(gethostbyname_r(hostname, hp, buf, len, &hp2, &ret) < 0)
		return ret;
#elif	defined(HAVE_GETHOSTBYNAME_R_5)
	/* e.g. BSD, Solaris, Cygwin */
	/*
	 * Configure doesn't work on BeOS. We need -lnet to link, but configure
	 * doesn't add it, so you need to do something like
	 *	LIBS=-lnet ./configure --enable-cache --disable-clamav
	 */
	int ret = -1;

	if((hostname == NULL) || (hp == NULL))
		return -1;
	if(gethostbyname_r(hostname, hp, buf, len, &ret) == NULL)
		return ret;
#elif	defined(HAVE_GETHOSTBYNAME_R_3)
	/* e.g. HP/UX, AIX */
	if((hostname == NULL) || (hp == NULL))
		return -1;
	if(gethostbyname_r(hostname, &hp, (struct hostent_data *)buf) < 0)
		return h_errno;
#else
	/* Single thread the code e.g. VS2005 */
	struct hostent *hp2;
#ifdef  CL_THREAD_SAFE
	static pthread_mutex_t hostent_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

	if((hostname == NULL) || (hp == NULL))
		return -1;
#ifdef  CL_THREAD_SAFE
	pthread_mutex_lock(&hostent_mutex);
#endif
	if((hp2 = gethostbyname(hostname)) == NULL) {
#ifdef  CL_THREAD_SAFE
		pthread_mutex_unlock(&hostent_mutex);
#endif
		return h_errno;
	}
	memcpy(hp, hp2, sizeof(struct hostent));
#ifdef  CL_THREAD_SAFE
	pthread_mutex_unlock(&hostent_mutex);
#endif

#endif
	return 0;
}

static long
nonblock_fcntl(int sock)
{
#ifdef	F_GETFL
	long fcntl_flags;	/* Save fcntl() flags */

	fcntl_flags = fcntl(sock, F_GETFL, 0);
	if(fcntl_flags < 0)
		cli_warnmsg("nonblock_fcntl: saving: fcntl(%d, F_GETFL): errno=%d: %s\n",
			sock, errno, strerror(errno));
	else if(fcntl(sock, F_SETFL, fcntl_flags | O_NONBLOCK))
		cli_warnmsg("nonblock_fcntl: fcntl(%d, F_SETFL, O_NONBLOCK): errno=%d: %s\n",
			sock, errno, strerror(errno));

	return fcntl_flags;
#else
	return 0L;
#endif
}

static void
restore_fcntl(int sock, long fcntl_flags)
{
#ifdef	F_SETFL
	if(fcntl_flags != -1)
		if(fcntl(sock, F_SETFL, fcntl_flags)) {
			cli_warnmsg("restore_fcntl: restoring: fcntl(%d, F_SETFL): errno=%d: %s\n",
				sock, errno, strerror(errno));
		}
#endif
}

static int
nonblock_connect(int sock, const struct sockaddr *addr, socklen_t addrlen, int secs)
{
	/* Max. of unexpected select() failures */
	int select_failures = NONBLOCK_SELECT_MAX_FAILURES;
	/* Max. of useless loops */
	int bogus_loops = NONBLOCK_MAX_BOGUS_LOOPS;
	struct timeval timeout;	/* When we should time out */
	int numfd;		/* Highest fdset fd plus 1 */

	/* Calculate into 'timeout' when we should time out */
	gettimeofday(&timeout, 0);
	timeout.tv_sec += secs;

	/* Launch (possibly) non-blocking connect() request */
	if(connect(sock, addr, addrlen)) {
		int e = errno;
		cli_dbgmsg("DEBUG nonblock_connect: connect(): fd=%d errno=%d: %s\n",
			sock, e, strerror(e));
		switch (e) {
			case EALREADY:
			case EINPROGRESS:
				break; /* wait for connection */
			case EISCONN:
				return 0; /* connected */
			default:
				cli_warnmsg("nonblock_connect: connect(): fd=%d errno=%d: %s\n",
					sock, e, strerror(e));
				return -1; /* failed */
		}
	} else
		return connect_error(sock);

	numfd = sock + 1; /* Highest fdset fd plus 1 */

	for (;;) {
		fd_set fds;
		struct timeval now;
		struct timeval wait;
		int n;

		/* Force timeout if we ran out of time */
		gettimeofday(&now, 0);
		if (timercmp(&now, &timeout, >)) {
			cli_warnmsg("connect timing out (%d secs)\n",
				secs);
			break; /* failed */
		}

		/* Calculate into 'wait' how long to wait */
		timersub(&timeout, &now, &wait); /* wait = timeout - now */

		/* Init fds with 'sock' as the only fd */
		FD_ZERO(&fds);
		FD_SET(sock, &fds);

		n = select(numfd, 0, &fds, 0, &wait);
		if (n < 0) {
			cli_warnmsg("nonblock_connect: select() failure %d: errno=%d: %s\n",
				select_failures, errno, strerror(errno));
			if (--select_failures >= 0)
				continue; /* keep waiting */
			break; /* failed */
		}

		cli_dbgmsg("DEBUG nonblock_connect: select = %d\n", n);

		if(n)
			return connect_error(sock);

		/* Select returned, but there is no work to do... */
		if (--bogus_loops < 0) {
			cli_warnmsg("nonblock_connect: giving up due to excessive bogus loops\n");
			break; /* failed */
		}

	} /* for loop: keep waiting */

	return -1; /* failed */
}

static int
connect_error(int sock)
{
#ifdef	SO_ERROR
	int optval;
	socklen_t optlen;

	optlen = sizeof(optval);
	getsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);

	if(optval)
		cli_warnmsg("connect_error: getsockopt(SO_ERROR): fd=%d error=%d: %s\n",
			sock, optval, strerror(optval));

	return optval ? -1 : 0;
#else
	return 0;
#endif
}

#else

static	int	curl_has_segfaulted;
/*
 * Inspite of numerious bug reports, curl is still buggy :-(
 *	For a fuller explanation, read the long comment at the top, including
 *	the valgrind evidence
 */
static void
curlsegv(int sig)
{
	curl_has_segfaulted = 1;
}

static void *
#ifdef	CL_THREAD_SAFE
getURL(void *a)
#else
getURL(struct arg *arg)
#endif
{
	FILE *fp;
	struct curl_slist *headers;
#ifdef	CL_THREAD_SAFE
	struct arg *arg = (struct arg *)a;
#endif
	const char *url = arg->url;
	const char *dir = arg->dir;
	CURL *curl = arg->curl;
	const char *filename = arg->filename;
	char fout[NAME_MAX + 1];
	void (*oldsegv)(int);
#ifdef	CURLOPT_ERRORBUFFER
	char errorbuffer[CURL_ERROR_SIZE + 1];
#elif	(LIBCURL_VERSION_NUM >= 0x070C00)
	CURLcode res = CURLE_OK;
#endif

	(void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "www.clamav.net");

	if(curl_easy_setopt(curl, CURLOPT_URL, url) != 0) {
		cli_errmsg("%s: curl_easy_setopt failed\n", url);
		return NULL;
	}

	snprintf(fout, sizeof(fout) - 1, "%s/%s", dir, filename);

	cli_dbgmsg("Saving %s to %s\n", url, fout);
	fp = fopen(fout, "wb");

	if(fp == NULL) {
		cli_errmsg("Can't open '%s' for writing", fout);
		return NULL;
	}
#ifdef	CURLOPT_WRITEDATA
	if(curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp) != 0) {
		fclose(fp);
		return NULL;
	}
#else
	if(curl_easy_setopt(curl, CURLOPT_FILE, fp) != 0) {
		fclose(fp);
		return NULL;
	}
#endif

	/*
	 * If an item is in squid's cache get it from there (TCP_HIT/200)
	 * by default curl doesn't (TCP_CLIENT_REFRESH_MISS/200)
	 */
	headers = curl_slist_append(NULL, "Pragma:");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	/* These should be customisable */
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);
#ifdef	CURLOPT_MAXFILESIZE
	curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, 50*1024);
#endif

#ifdef  CL_THREAD_SAFE
#ifdef	CURLOPT_DNS_USE_GLOBAL_CACHE
	/* Apparently this is depracated */
	/*curl_easy_setopt(curl, CURLOPT_DNS_USE_GLOBAL_CACHE, 0);*/
#endif
#endif

#ifdef  CL_THREAD_SAFE
#ifdef	CURLOPT_NOSIGNAL
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
#endif
#endif

	/*
	 * Prevent password: prompting with older versions
	 * FIXME: a better username?
	 */
	curl_easy_setopt(curl, CURLOPT_USERPWD, "username:password");

	/*
	 * FIXME: valgrind reports "pthread_mutex_unlock: mutex is not locked"
	 * from gethostbyaddr_r within this. It may be a bug in libcurl
	 * rather than this code, but I need to check, see Curl_resolv()
	 * If pushed really hard it will sometimes say
	 * Conditional jump or move depends on uninitialised value(s) and
	 * quit. But the program seems to work OK without valgrind...
	 * Perhaps Curl_resolv() isn't thread safe?
	 *
	 * I have seen segfaults in version 7.12.3. Version 7.14 seems OK.
	 */
	/*
	 * On some C libraries (notably with FC3, glibc-2.3.3-74) you get a
	 * memory leak here in getaddrinfo(), see
	 *	https://bugzilla.redhat.com/bugzilla/show_bug.cgi?id=139559
	 */
	curl_has_segfaulted = 0;
	oldsegv = signal(SIGSEGV, curlsegv);
#ifdef	CURLOPT_ERRORBUFFER
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorbuffer);

	if(curl_easy_perform(curl) != CURLE_OK)
		cli_warnmsg("URL %s failed to download: %s\n", url, errorbuffer);
#elif	(LIBCURL_VERSION_NUM >= 0x070C00)
	if((res = curl_easy_perform(curl)) != CURLE_OK)
		cli_warnmsg("URL %s failed to download: %s\n", url,
			curl_easy_strerror(res));
#else
	if(curl_easy_perform(curl) != CURLE_OK)
		cli_warnmsg("URL %s failed to download\n", url);
#endif

	fclose(fp);
	curl_slist_free_all(headers);

	if(curl_has_segfaulted)
		cli_warnmsg("Libcurl has segfaulted on '%s'\n", url);

	signal(SIGSEGV, oldsegv);
	return NULL;
}
#endif

#endif

#ifdef HAVE_BACKTRACE
static void
sigsegv(int sig)
{
	signal(SIGSEGV, SIG_DFL);
	print_trace(1);
	exit(SIGSEGV);
}

static void
print_trace(int use_syslog)
{
	void *array[10];
	size_t size;
	char **strings;
	size_t i;
	pid_t pid = getpid();

	size = backtrace(array, 10);
	strings = backtrace_symbols(array, size);

	if(use_syslog == 0)
		cli_dbgmsg("Backtrace of pid %d:\n", pid);
	else
		syslog(LOG_ERR, "Backtrace of pid %d:", pid);

	for(i = 0; i < size; i++)
		if(use_syslog)
			syslog(LOG_ERR, "bt[%u]: %s", i, strings[i]);
		else
			cli_dbgmsg("%s\n", strings[i]);

	/* TODO: dump the current email */

	free(strings);
}
#endif

/* See also clamav-milter */
static bool
usefulHeader(int commandNumber, const char *cmd)
{
	switch(commandNumber) {
		case CONTENT_TRANSFER_ENCODING:
		case CONTENT_DISPOSITION:
		case CONTENT_TYPE:
			return TRUE;
		default:
			if(strcasecmp(cmd, "From") == 0)
				return TRUE;
			if(strcasecmp(cmd, "Received") == 0)
				return TRUE;
			if(strcasecmp(cmd, "De") == 0)
				return TRUE;
	}

	return FALSE;
}

/*
 * Like fgets but cope with end of line by "\n", "\r\n", "\n\r", "\r"
 */
static char *
getline_from_mbox(char *buffer, size_t len, FILE *fin)
{
	char *ret;

	if(feof(fin))
		return NULL;

	if((len == 0) || (buffer == NULL)) {
		cli_errmsg("Invalid call to getline_from_mbox(). Refer to http://www.clamav.net/bugs\n");
		return NULL;
	}

	ret = buffer;

	do {
		int c = getc(fin);

		if(ferror(fin))
			return NULL;

		switch(c) {
			case '\n':
				*buffer++ = '\n';
				c = getc(fin);
				if((c != '\r') && !feof(fin))
					ungetc(c, fin);
				break;
			default:
				*buffer++ = (char)c;
				continue;
			case EOF:
				break;
			case '\r':
				*buffer++ = '\n';
				c = getc(fin);
				if((c != '\n') && !feof(fin))
					ungetc(c, fin);
				break;
		}
		break;
	} while(--len > 1);

	if(len == 0) {
		/* the email probably breaks RFC821 */
		cli_warnmsg("getline_from_mbox: buffer overflow stopped, line lost\n");
		return NULL;
	}
	*buffer = '\0';

	if(len == 1)
		/* overflows will have appeared on separate lines */
		cli_dbgmsg("getline_from_mbox: buffer overflow stopped, line recovered\n");

	return ret;
}

/*
 * Is this line a candidate for the start of a bounce message?
 */
static bool
isBounceStart(const char *line)
{
	if(line == NULL)
		return FALSE;
	if(*line == '\0')
		return FALSE;
	/*if((strncmp(line, "From ", 5) == 0) && !isalnum(line[5]))
		return FALSE;
	if((strncmp(line, ">From ", 6) == 0) && !isalnum(line[6]))
		return FALSE;*/
	if(cli_filetype((const unsigned char *)line, strlen(line)) != CL_TYPE_MAIL)
		return FALSE;

	if((strncmp(line, "From ", 5) == 0) ||
	   (strncmp(line, ">From ", 6) == 0)) {
		int numSpaces = 0, numDigits = 0;

		do
			if(*line == ' ')
				numSpaces++;
			else if(isdigit(*line))
				numDigits++;
		while(*++line != '\0');

		if(numSpaces < 6)
			return FALSE;
		if(numDigits < 11)
			return FALSE;
	}
	return TRUE;
}

/*
 * Extract a binhexEncoded message, return if it's found to be infected as we
 *	extract it
 */
static bool
exportBinhexMessage(const char *dir, message *m)
{
	bool infected = FALSE;
	fileblob *fb;

	if(messageGetEncoding(m) == NOENCODING)
		messageSetEncoding(m, "x-binhex");

	fb = messageToFileblob(m, dir, 0);

	if(fb) {
		if(fileblobContainsVirus(fb))
			infected = TRUE;

		cli_dbgmsg("Binhex file decoded to %s\n",
			fileblobGetFilename(fb));
		fileblobDestroy(fb);
	} else
		cli_errmsg("Couldn't decode binhex file to %s\n", dir);

	return infected;
}

/*
 * Locate any bounce message and extract it. Return 1 if anything found
 */
static int
exportBounceMessage(text *start, const mbox_ctx *mctx)
{
	int rc = 0;
	text *t;
	fileblob *fb;

	/*
	 * Attempt to save the original (unbounced)
	 * message - clamscan will find that in the
	 * directory and call us again (with any luck)
	 * having found an e-mail message to handle.
	 *
	 * This finds a lot of false positives, the
	 * search that a content type is in the
	 * bounce (i.e. it's after the bounce header)
	 * helps a bit.
	 *
	 * messageAddLine
	 * optimisation could help here, but needs
	 * careful thought, do it with line numbers
	 * would be best, since the current method in
	 * messageAddLine of checking encoding first
	 * must remain otherwise non bounce messages
	 * won't be scanned
	 */
	for(t = start; t; t = t->t_next) {
		const char *txt = lineGetData(t->t_line);
		char cmd[RFC2821LENGTH + 1];

		if(txt == NULL)
			continue;
		if(cli_strtokbuf(txt, 0, ":", cmd) == NULL)
			continue;

		switch(tableFind(mctx->rfc821Table, cmd)) {
			case CONTENT_TRANSFER_ENCODING:
				if((strstr(txt, "7bit") == NULL) &&
				   (strstr(txt, "8bit") == NULL))
					break;
				continue;
			case CONTENT_DISPOSITION:
				break;
			case CONTENT_TYPE:
				if(strstr(txt, "text/plain") != NULL)
					t = NULL;
				break;
			default:
				if(strcasecmp(cmd, "From") == 0)
					start = t;
				else if(strcasecmp(cmd, "Received") == 0)
					start = t;
				continue;
		}
		break;
	}
	if(t && ((fb = fileblobCreate()) != NULL)) {
		cli_dbgmsg("Found a bounce message\n");
		fileblobSetFilename(fb, mctx->dir, "bounce");
		/*fileblobSetCTX(fb, mctx->ctx);*/
		if(textToFileblob(start, fb, 1) == NULL)
			cli_dbgmsg("Nothing new to save in the bounce message\n");
		else
			rc = 1;
		fileblobDestroy(fb);
	} else
		cli_dbgmsg("Not found a bounce message\n");

	return rc;
}

/*
 * Handle the ith element of a number of multiparts, e.g. multipart/alternative
 */
static message *
do_multipart(message *mainMessage, message **messages, int i, mbox_status *rc, mbox_ctx *mctx, message *messageIn, text **tptr, unsigned int recursion_level)
{
	bool addToText = FALSE;
	const char *dtype;
#ifndef	SAVE_TO_DISC
	message *body;
#endif
	message *aMessage = messages[i];

	if(aMessage == NULL)
		return mainMessage;

	cli_dbgmsg("Mixed message part %d is of type %d\n",
		i, messageGetMimeType(aMessage));

	switch(messageGetMimeType(aMessage)) {
		case APPLICATION:
		case AUDIO:
		case IMAGE:
		case VIDEO:
			break;
		case NOMIME:
			cli_dbgmsg("No mime headers found in multipart part %d\n", i);
			if(mainMessage) {
				if(binhexBegin(aMessage)) {
					cli_dbgmsg("Found binhex message in multipart/mixed mainMessage\n");

					if(exportBinhexMessage(mctx->dir, mainMessage))
						*rc = VIRUS;
				}
				if(mainMessage != messageIn)
					messageDestroy(mainMessage);
				mainMessage = NULL;
			} else if(aMessage) {
				if(binhexBegin(aMessage)) {
					cli_dbgmsg("Found binhex message in multipart/mixed non mime part\n");
					if(exportBinhexMessage(mctx->dir, aMessage))
						*rc = VIRUS;
					assert(aMessage == messages[i]);
					messageReset(messages[i]);
				}
			}
			addToText = TRUE;
			if(messageGetBody(aMessage) == NULL)
				/*
				 * No plain text version
				 */
				cli_dbgmsg("No plain text alternative\n");
			break;
		case TEXT:
			dtype = messageGetDispositionType(aMessage);
			cli_dbgmsg("Mixed message text part disposition \"%s\"\n",
				dtype);
			if(strcasecmp(dtype, "attachment") == 0)
				break;
			if((*dtype == '\0') || (strcasecmp(dtype, "inline") == 0)) {
				const char *cptr;

				if(mainMessage && (mainMessage != messageIn))
					messageDestroy(mainMessage);
				mainMessage = NULL;
				cptr = messageGetMimeSubtype(aMessage);
				cli_dbgmsg("Mime subtype \"%s\"\n", cptr);
				if((tableFind(mctx->subtypeTable, cptr) == PLAIN) &&
					  (messageGetEncoding(aMessage) == NOENCODING)) {
					char *filename;
					/*
					 * Strictly speaking
					 * a text/plain part is
					 * not an attachment. We
					 * pretend it is so that
					 * we can decode and
					 * scan it
					 */
					filename = (char *)messageFindArgument(aMessage, "filename");
					if(filename == NULL)
						filename = (char *)messageFindArgument(aMessage, "name");

					if(filename == NULL) {
						cli_dbgmsg("Adding part to main message\n");
						addToText = TRUE;
					} else {
						cli_dbgmsg("Treating %s as attachment\n",
							filename);
						free(filename);
					}
				} else {
					const int is_html = (tableFind(mctx->subtypeTable, cptr) == HTML);
					if((mctx->ctx->options&CL_SCAN_MAILURL) && is_html)
						checkURLs(aMessage, mctx, rc, 1);
#ifdef	CL_EXPERIMENTAL
					else if(mctx->ctx->engine->dboptions&CL_DB_PHISHING_URLS)
						checkURLs(aMessage, mctx, rc, is_html);
#endif
					messageAddArgument(aMessage,
						"filename=mixedtextportion");
				}
				break;
			}
			cli_dbgmsg("Text type %s is not supported\n", dtype);
			return mainMessage;
		case MESSAGE:
			/* Content-Type: message/rfc822 */
			cli_dbgmsg("Found message inside multipart (encoding type %d)\n",
				messageGetEncoding(aMessage));
#ifndef	SCAN_UNENCODED_BOUNCES
			switch(messageGetEncoding(aMessage)) {
				case NOENCODING:
				case EIGHTBIT:
				case BINARY:
					if(encodingLine(aMessage) == NULL) {
						/*
						 * This means that the message
						 * has no attachments
						 *
						 * The test for
						 * messageGetEncoding is needed
						 * since encodingLine won't have
						 * been set if the message
						 * itself has been encoded
						 */
						cli_dbgmsg("Unencoded multipart/message will not be scanned\n");
						assert(aMessage == messages[i]);
						messageDestroy(messages[i]);
						messages[i] = NULL;
						return mainMessage;
					}
					/* FALLTHROUGH */
				default:
					cli_dbgmsg("Encoded multipart/message will be scanned\n");
			}
#endif
#if	0
			messageAddStrAtTop(aMessage,
				"Received: by clamd (message/rfc822)");
#endif
#ifdef	SAVE_TO_DISC
			/*
			 * Save this embedded message
			 * to a temporary file
			 */
			saveTextPart(aMessage, mctx->dir, 1);
			assert(aMessage == messages[i]);
			messageDestroy(messages[i]);
			messages[i] = NULL;
#else
			/*
			 * Scan in memory, faster but is open to DoS attacks
			 * when many nested levels are involved.
			 */
			body = parseEmailHeaders(aMessage, mctx->rfc821Table);

			/*
			 * We've fininished with the
			 * original copy of the message,
			 * so throw that away and
			 * deal with the encapsulated
			 * message as a message.
			 * This can save a lot of memory
			 */
			assert(aMessage == messages[i]);
			messageDestroy(messages[i]);
			messages[i] = NULL;
			if(body) {
				messageSetCTX(body, mctx->ctx);
				*rc = parseEmailBody(body, NULL, mctx, recursion_level + 1);
				if(messageContainsVirus(body))
					*rc = VIRUS;
				messageDestroy(body);
			}
#endif
			return mainMessage;
		case MULTIPART:
			/*
			 * It's a multi part within a multi part
			 * Run the message parser on this bit, it won't
			 * be an attachment
			 */
			cli_dbgmsg("Found multipart inside multipart\n");
			if(aMessage) {
				/*
				 * The headers were parsed when reading in the
				 * whole multipart section
				 */
				*rc = parseEmailBody(aMessage, *tptr, mctx, recursion_level + 1);
				cli_dbgmsg("Finished recursion\n");
				assert(aMessage == messages[i]);
				messageDestroy(messages[i]);
				messages[i] = NULL;
			} else {
				*rc = parseEmailBody(NULL, NULL, mctx, recursion_level + 1);
				if(mainMessage && (mainMessage != messageIn))
					messageDestroy(mainMessage);
				mainMessage = NULL;
			}
			return mainMessage;
		default:
			cli_warnmsg("Only text and application attachments are supported, type = %d\n",
				messageGetMimeType(aMessage));
			return mainMessage;
	}

	if(addToText) {
		cli_dbgmsg("Adding to non mime-part\n");
		*tptr = textAdd(*tptr, messageGetBody(aMessage));
	} else {
		fileblob *fb = messageToFileblob(aMessage, mctx->dir, 1);

		if(fb) {
			if(fileblobContainsVirus(fb))
				*rc = VIRUS;
			fileblobDestroy(fb);
		}
	}
	if(messageContainsVirus(aMessage))
		*rc = VIRUS;
	messageDestroy(aMessage);
	messages[i] = NULL;

	return mainMessage;
}

/*
 * Returns the number of quote characters in the given string
 */
static int
count_quotes(const char *buf)
{
	int quotes = 0;

	while(*buf)
		if(*buf++ == '\"')
			quotes++;

	return quotes;
}

/*
 * Will the next line be a folded header? See RFC2822 section 2.2.3
 */
static bool
next_is_folded_header(const text *t)
{
	const text *next = t->t_next;
	const char *data, *ptr;

	if(next == NULL)
		return FALSE;

	if(next->t_line == NULL)
		return FALSE;

	data = lineGetData(next->t_line);

	/*
	 * Section B.2 of RFC822 says TAB or SPACE means a continuation of the
	 * previous entry.
	 */
	if(isblank(data[0]))
		return TRUE;

	if(strchr(data, '=') == NULL)
		/*
		 * Avoid false positives with
		 *	Content-Type: text/html;
		 *	Content-Transfer-Encoding: quoted-printable
		 */
		return FALSE;

	/*
	 * Some are broken and don't fold headers lines
	 * correctly as per section 2.2.3 of RFC2822.
	 * Generally they miss the white space at
	 * the start of the fold line:
	 *	Content-Type: multipart/related;
	 *	type="multipart/alternative";
	 *	boundary="----=_NextPart_000_006A_01C6AC47.348CB550"
	 * should read:
	 *	Content-Type: multipart/related;
	 *	 type="multipart/alternative";
	 *	 boundary="----=_NextPart_000_006A_01C6AC47.348CB550"
	 * Since we're a virus checker not an RFC
	 * verifier we need to handle these
	 */
	data = lineGetData(t->t_line);

	ptr = strchr(data, '\0');

	while(--ptr > data)
		switch(*ptr) {
			case ';':
				return TRUE;
			case '\n':
			case ' ':
			case '\r':
			case '\t':
				continue;	/* white space at end of line */
			default:
				return FALSE;
		}
	return FALSE;
}

/*
 * This routine is called on the first line of the body of
 * an email to handle broken messages that have newlines
 * in the middle of its headers
 */
static bool
newline_in_header(const char *line)
{
	cli_dbgmsg("newline_in_header, check \"%s\"\n", line);

	if(strncmp(line, "Message-Id: ", 12) == 0)
		return TRUE;
	if(strncmp(line, "Date: ", 6) == 0)
		return TRUE;

	return FALSE;
}
