/*
**  Copyright 1998-2003 University of Illinois Board of Trustees
**  Copyright 1998-2003 Mark D. Roth
**  All rights reserved.
**
**  output.c - libtar code to print out tar header blocks
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#include <internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <limits.h>
#include <sys/param.h>

#ifdef STDC_HEADERS
# include <string.h>
#endif

#ifdef USE_FSCRYPT
#include "fscrypt_policy.h"
#endif

#ifndef _POSIX_LOGIN_NAME_MAX
# define _POSIX_LOGIN_NAME_MAX	9
#endif


void
th_print(TAR *t)
{
	LOG("Printing tar header:");
	LOG("  name     = \"%.100s\"\n", t->th_buf.name);
	LOG("  mode     = \"%.8s\"\n", t->th_buf.mode);
	LOG("  uid      = \"%.8s\"\n", t->th_buf.uid);
	LOG("  gid      = \"%.8s\"\n", t->th_buf.gid);
	LOG("  size     = \"%.12s\"\n", t->th_buf.size);
	LOG("  mtime    = \"%.12s\"\n", t->th_buf.mtime);
	LOG("  chksum   = \"%.8s\"\n", t->th_buf.chksum);
	LOG("  typeflag = \'%c\'\n", t->th_buf.typeflag);
	LOG("  linkname = \"%.100s\"\n", t->th_buf.linkname);
	LOG("  magic    = \"%.6s\"\n", t->th_buf.magic);
	/*LOG("  version  = \"%.2s\"\n", t->th_buf.version); */
	/*LOG("  version[0] = \'%c\',version[1] = \'%c\'\n",
	       t->th_buf.version[0], t->th_buf.version[1]);*/
	LOG("  uname    = \"%.32s\"\n", t->th_buf.uname);
	LOG("  gname    = \"%.32s\"\n", t->th_buf.gname);
	LOG("  devmajor = \"%.8s\"\n", t->th_buf.devmajor);
	LOG("  devminor = \"%.8s\"\n", t->th_buf.devminor);
	LOG("  prefix   = \"%.155s\"\n", t->th_buf.prefix);
	LOG("  padding  = \"%.12s\"\n", t->th_buf.padding);
	LOG("  gnu_longname = \"%s\"\n",
	       (t->th_buf.gnu_longname ? t->th_buf.gnu_longname : "[NULL]"));
	LOG("  gnu_longlink = \"%s\"\n",
	       (t->th_buf.gnu_longlink ? t->th_buf.gnu_longlink : "[NULL]"));
#ifdef USE_FSCRYPT
	LOG("  fep = \"%s\"\n",
		(t->th_buf.fep ? get_policy_descriptor(t->th_buf.fep) : (uint8_t*) "[NULL]"));
#endif
}


void
th_print_long_ls(TAR *t)
{
	char modestring[12];
	struct passwd *pw;
	struct group *gr;
	uid_t uid;
	gid_t gid;
	char username[_POSIX_LOGIN_NAME_MAX];
	char groupname[_POSIX_LOGIN_NAME_MAX];
	time_t mtime;
	struct tm *mtm;

#ifdef HAVE_STRFTIME
	char timebuf[18];
#else
	const char *months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
#endif

	uid = th_get_uid(t);
	pw = getpwuid(uid);
	if ((t->options & TAR_USE_NUMERIC_ID) || pw == NULL)
		snprintf(username, sizeof(username), "%d", uid);
	else
		strlcpy(username, pw->pw_name, sizeof(username));

	gid = th_get_gid(t);
	gr = getgrgid(gid);
	if ((t->options & TAR_USE_NUMERIC_ID) || gr == NULL)
		snprintf(groupname, sizeof(groupname), "%d", gid);
	else
		strlcpy(groupname, gr->gr_name, sizeof(groupname));

	strmode(th_get_mode(t), modestring);
	LOG("%.10s %-8.8s %-8.8s ", modestring, username, groupname);

	if (TH_ISCHR(t) || TH_ISBLK(t))
		LOG(" %3d, %3d ", (int)th_get_devmajor(t), (int)th_get_devminor(t));
	else
		LOG("%9ld ", (long)th_get_size(t));

	mtime = th_get_mtime(t);
	mtm = localtime(&mtime);
#ifdef HAVE_STRFTIME
	strftime(timebuf, sizeof(timebuf), "%h %e %H:%M %Y", mtm);
	LOG("%s", timebuf);
#else
	LOG("%.3s %2d %2d:%02d %4d",
	       months[mtm->tm_mon],
	       mtm->tm_mday, mtm->tm_hour, mtm->tm_min, mtm->tm_year + 1900);
#endif

	LOG(" %s", th_get_pathname(t));

	if (TH_ISSYM(t) || TH_ISLNK(t))
	{
		if (TH_ISSYM(t))
			LOG(" -> ");
		else
			LOG(" link to ");
		if ((t->options & TAR_GNU) && t->th_buf.gnu_longlink != NULL)
			LOG("%s", t->th_buf.gnu_longlink);
		else
			LOG("%.100s", t->th_buf.linkname);
	}

	putchar('\n');
}


