/*
 * PMWEBD graphite-api emulation
 *
 * Copyright (c) 2014-2017 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#define _XOPEN_SOURCE 600

#include "pmwebapi.h"

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <set>
#include <stack>
#include <map>

using namespace std;

extern "C"
{
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#ifdef HAVE_FTS_H
#include <fts.h>
#endif
#include <fnmatch.h>
#include <regex.h>
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif
#ifdef HAVE_IEEEFP_H
#include <ieeefp.h>
#endif
#ifdef HAVE_MATH_H
#include <math.h>
#endif
#ifdef HAVE_CAIRO
#include <cairo/cairo.h>
#endif
}



/*
 * We need a reversible encoding from arbitrary non-empty strings
 * (such as archive path names, pcp metric name components (?), pcp
 * instance names) to the dot-separated components of graphite metric
 * names.  We try to preserve safe characters and encode the rest.
 * (The resulting strings may well be url-encoded eventually for
 * transport across GET or POST parameters etc., but that's none of
 * our concern.)
 */
string
pmgraphite_metric_encode (const string & foo)
{
    stringstream output;
    static const char hex[] = "0123456789ABCDEF";

    assert (foo.size () > 0);
    for (unsigned i = 0; i < foo.size (); i++) {
        char c = foo[i];
        // Pass through enough characters to make the metric names relatively
        // human-readable in the javascript guis
        if (graphite_encode) {
            if (isalnum (c) || (c == '_') || (c == ' ')) {
                output << c;
            } else {
                output << "-" << hex[(c >> 4) & 15] << hex[(c >> 0) & 15] << "-";
            }
        }
        // Newer graphite/grafana support more characters
        else {
            if (isalnum (c) || (c == '_') || (c == ' ') || (c == '-') || (c == '/') ) {
                output << c;
            } else {
                // NB: ~hex rather than %hex to reduce likelihood of
                // triggering latent url-quoting bugs in webapps (graphite->graphlot),
                // and to avoid necessitating urlencoding of the ~hex code itself.
                output << "~" << hex[(c >> 4) & 15] << hex[(c >> 0) & 15];
            }
        }
    }
    return output.str ();
}

// NB: decoding failure is possible (could arise from mischevious URLs
// being fed to us) and is signalled with an empty return string.
string
pmgraphite_metric_decode (const string & foo)
{
    string output;
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < foo.size (); i++) {
        char c = foo[i];
        if (graphite_encode) {
            if (c == '-') {
                if (i + 3 >= foo.size ()) {
                    return "";
                }
                const char *p = lower_bound (hex, hex + 16, foo[i + 1]);
                if (*p != foo[i + 1]) {
                    return "";
                }
                const char *q = lower_bound (hex, hex + 16, foo[i + 2]);
                if (*q != foo[i + 2]) {
                    return "";
                }
                if ('-' != foo[i + 3]) {
                    return "";
                }
                output += (char) (((p - hex) << 4) | (q - hex));
                i += 3;		// skip over hex bytes
            } else {
                output += c;
            }
        } else {
            if (c == '~') {
                if (i + 2 >= foo.size ()) {
                    return "";
                }
                const char *p = lower_bound (hex, hex + 16, foo[i + 1]);
                if (*p != foo[i + 1]) {
                    return "";
                }
                const char *q = lower_bound (hex, hex + 16, foo[i + 2]);
                if (*q != foo[i + 2]) {
                    return "";
                }
                output += (char) (((p - hex) << 4) | (q - hex));
                i += 2;		// skip over hex bytes
            } else {
                output += c;
            }
        }
    }
    return output;
}


// An encoding function that can be one-way (irreversible) and pretty
// human-readable.
string
pmgraphite_metric_encode_1way (const string & foo)
{
    stringstream output;
    for (unsigned i = 0; i < foo.size (); i++) {
        char c = foo[i];
        if (isalnum (c) || (c == '_') || (c == ' ') || (c == '-') || (c == '/') )
            output << c;
        else
            output << "_";
    }

    return output.str ();
}




// ------------------------------------------------------------------------


// Represent a graphite metric name (segment) with this POD-dy object.
//
// We store a "head" and "tail" as optional separate flyweight pieces,
// because we want to minimize overall storage.  Concatenating
// head+tail and flyweight-storing that as a unit could cancel out
// much of the memory savings opportunity.  Storing a
// vector<flyweight_string> chunks also negates the savings, as the
// vector and the pointer can take as much space as the whole string.
//
struct metric_string {
    flyweight_string head;
    flyweight_string tail; // may be empty
    metric_string(const string& x): head(x), tail("") {}
    metric_string(const string& x, const string& y): head(x), tail(y) {}
    metric_string(const string& x, const metric_string& y): head(x), tail(y.unsplit()) {}

    // We don't want to routinely store these values - that'd defeat
    // the purpose.  We trade computation for storage.  (Maybe
    // memoization for a subset would make sense.)
    string unsplit() const;
    vector<string> split() const;
    size_t split_size() const;
};
// frequently used alias
typedef vector<metric_string>::iterator mvi_t;

string metric_string::unsplit() const
{
    string s = head;
    if (tail != "")
        s += string(".") + string(tail);
    return s;
}
vector<string> metric_string::split() const
{
    return ::split(unsplit(), '.');
}
size_t metric_string::split_size() const
{
    const string x = unsplit();
    return count(x.begin(), x.end(), '.') + 1;
}

// needed for sorted storage in a set
inline bool operator < (const metric_string& a, const metric_string& b)
{
    return (a.head < b.head) ||
        ((a.head == b.head) && (a.tail < b.tail));
}
inline bool operator == (const metric_string& a, const metric_string& b)
{
    return (a.head == b.head) && (a.tail == b.tail);
}


typedef multimap<pmInDom,string> pmis_t;

struct pmg_enum_context {
    vector<metric_string> *output;
    pmis_t indom_instance_parts; // filtered indom instance names
};



// Callback from pmTraversePMNS_r.  We have a working archive, we just received
// a working metric name.  All we need now is to check the metric for compatibility
// with graphite, and save its name into the incoming context vector.
void
pmg_enumerate_pmns (const char *name, void *cls)
{
    pmg_enum_context *c = (pmg_enum_context *) cls;
    string metricpart = name;

    if (exit_p)
        return;

    // look up the metric to make sure it exists; fan out to instance domains while at it
    char *namelist[1];
    pmID pmidlist[1];
    namelist[0] = (char *) name;
    int sts = pmLookupName (1, namelist, pmidlist);
    if (sts != 1) {
        return;
    }

    pmID pmid = pmidlist[0];
    pmDesc pmd;
    sts = pmLookupDesc (pmid, &pmd);
    if (sts != 0) {
        return;    // should not happen
    }

    // Filter out non-numeric metrics
    switch (pmd.type) {
    case PM_TYPE_32:
    case PM_TYPE_U32:
    case PM_TYPE_64:
    case PM_TYPE_U64:
    case PM_TYPE_FLOAT:
    case PM_TYPE_DOUBLE:
        break; // fall through
    default:
        return;
    }

    if (pmd.indom == PM_INDOM_NULL) { // no more
        c->output->push_back (metric_string(metricpart));
    } else { // has instance domain - get one more graphite name component
        // check indom instance cache
        if (c->indom_instance_parts.find(pmd.indom) == c->indom_instance_parts.end()) {
            // populate it
            int *instlist;
            char **namelist;
            sts = pmGetInDomArchive (pmd.indom, &instlist, &namelist);
            if (sts >= 1) {
                for (int i=0; i<sts; i++) {
                    string instance_part = pmgraphite_metric_encode (namelist[i]);
                    c->indom_instance_parts.insert(make_pair(pmd.indom, instance_part));
                }
                free (instlist);
                free (namelist);
            } else {
                // should not happen
            }
        }

        // iterate across instance cache
        pair<pmis_t::iterator,pmis_t::iterator> range = c->indom_instance_parts.equal_range(pmd.indom);
        for (pmis_t::iterator a = range.first; a != range.second; a++) {
            c->output->push_back (metric_string(metricpart, a->second));
        }
    }
}


// Heavy lifter.  Enumerate all archives, all metrics, all instances.
// In theory, this is not unbearably slow, since it involves only a
// scan of directories & metadata.... but on a large system, when we
// have thousands of archives, not enough RAM, yes it can be slow.
//
// So we cache metadata and start/end timestamps from archives, so as
// to avoid even opening them with pmNewContext to get this data.  That
// only works if the cache's freshness can be checked, so we have some
// fstat keys.

struct archivecache_entry {
    // permanent values
    string filename; // to feed to pmNewContext to refresh cache
    string archivepart; // the graphite metric component
    struct timeval archive_begin;

    // metrics cache
    vector<metric_string> metrics; 
    time_t metadata_mtime;

    // archive-end-time cache
    struct timeval archive_end;
    unsigned archive_lastvol_idx;
    time_t archive_lastvol_mtime;

    // last refresh time
    time_t last_refresh_time;
};


//typedef set<archivecache_entry*> archivecache_t;
//archivecache_t archivecache;
typedef map<string,archivecache_entry*> ac_by_fn_t;
ac_by_fn_t archivecache_by_filename;
typedef multimap<string,archivecache_entry*> ac_by_ap_t;
ac_by_ap_t archivecache_by_archivepart;



// Compute an "archivepart" for the given archive file name (.meta or
// dir/), already opened with given pcp archive context.  This can be
// an encoded version of the file name, or the pcp hostname found
// therein (depending on pmwebd mode flags).
static string
ac_archivepart (const string& filename, int ctx)
{
    string archivepart;
    if (graphite_hostcache) {
        char pcp_hostname[MAXHOSTNAMELEN];
        (void) pmGetContextHostName_r (ctx, pcp_hostname, sizeof(pcp_hostname));

        archivepart = pmgraphite_metric_encode_1way (pcp_hostname);
    } else {
        archivepart = filename;

        // Abbrevate archive to clip off the archivesdir prefix (if
        // it's there).
        if (has_prefix(archivepart,archivesdir))
            archivepart = archivepart.substr(archivesdir.size() + 1);

        // Remove the .meta part
        if (!graphite_encode) {
            string metastring = ".meta";
            string::size_type metaidx = archivepart.rfind(metastring);
            if (metaidx != std::string::npos) // unlikely to fail, due to fnmatch glob pattern
                archivepart.erase(metaidx, metastring.length());
        }

        archivepart = pmgraphite_metric_encode (archivepart);
    }

    return archivepart;
}


// Given a PCP archive name, create (if new) or refresh (if needed) its
// data in the archivecache.  If successful, we can count on it showing
// up in the archivecache.  If unsuccessful, print an error message,
// and possibly remove all traces of the entry from the archivecache.
static void
ac_refresh(struct MHD_Connection * connection, const string& filename)
{
    int rc = 0;
    archivecache_entry *e = 0;
    char pmmsg[PM_MAXERRMSGLEN];
    int ctx = -1;
    time_t now = time(NULL);

    // find our cache entry
    ac_by_fn_t::iterator it = archivecache_by_filename.find(filename);
    if (it != archivecache_by_filename.end())
        e = it->second;

    // clean up
    if (e && exit_p) {
        archivecache_by_filename.erase(filename);
        // the multimaps are harder
        pair<ac_by_ap_t::iterator,ac_by_ap_t::iterator> its =
            archivecache_by_archivepart.equal_range(e->archivepart);
        for (ac_by_ap_t::iterator it = its.first; it != its.second; it++)
            if (it->second->filename == filename) { archivecache_by_archivepart.erase(it); break; }
        // see what I mean?
        // taps
        delete e;
        return;
    }
    
    if (! e) { // not in cache yet - a new archive file
        // Fill in all permanent parts of the entry here, metrics/archive-end soon
        e = new archivecache_entry;

        e->filename = filename;
        ctx = pmNewContext (PM_CONTEXT_ARCHIVE, filename.c_str ());
        if (ctx < 0) {
            delete e;
            connstamp (cerr, connection) << "cannot open " << filename << ": "
                                         << pmErrStr_r (ctx, pmmsg, sizeof (pmmsg))
                                         << endl;
            return;
        }
        pmLogLabel l;
        rc = pmGetArchiveLabel(& l);
        if (rc < 0) {
            delete e;
            connstamp (cerr, connection) << "cannot get archive log label " << filename << ": "
                                         << pmErrStr_r (rc, pmmsg, sizeof (pmmsg))
                                         << endl;
            return;
        }
        e->archive_begin = l.ll_start;
        e->archivepart = ac_archivepart (filename, ctx);
        e->archive_end.tv_sec = std::numeric_limits<time_t>::max();
        e->archive_end.tv_usec = 0;
 
        // invalidate the caches
        e->metadata_mtime = 0;
        e->archive_lastvol_mtime = 0;
        e->archive_lastvol_idx = 0;
        e->last_refresh_time = 0;

        archivecache_by_filename.insert(make_pair(e->filename,e));
        archivecache_by_archivepart.insert(make_pair(e->archivepart,e));
    }

    // If this is an ancient archive, assume that it will not undergo
    // any further change "soon", so eschew frequent freshness checking.
    // The criterion for "soon" depends on the age of the archive
    // (its archive_lastvol_mtime) and the time of the most recent
    // refresh.  The difference interval is multiplied by a scale
    // factor, to create an assumed-fresh interval.  If that assumed-fresh
    // interval includes the present moment, then we assume the archive
    // is still fresh.  (This is similar to way the squid http cache
    // calculates freshness if no other data is available.)
    //
    // This is a performance tradeoff.  Where an archivecache_entry is
    // assumed fresh but has changed or disappeared, at worst we offer
    // temporarily incomplete data to a webapp.
    //
    if (e->last_refresh_time && e->archive_lastvol_mtime) { // been through a complete refresh
        time_t age_at_last_refresh = e->last_refresh_time - e->archive_lastvol_mtime;
        float freshness_ratio = 0.50; // XXX: parametrize this default
        time_t assume_fresh_until = e->last_refresh_time + (freshness_ratio * age_at_last_refresh);
        if (now < assume_fresh_until)
            return;
    }

    // Check freshness of metrics - consume one stat().  We shouldn't
    // use the graphite_oldness heuristic here, because .meta files
    // can easily be much older than an active archive volume, if for
    // example the set of metrics & their indoms don't change after
    // logger startup.

    struct stat st;
    rc = stat(filename.c_str(), &st);
    if (rc < 0) {
        // the .meta file has disappeared - retire this archivecache_entry!
        // the map is easy
        archivecache_by_filename.erase(filename);
        // the multimaps are harder
        pair<ac_by_ap_t::iterator,ac_by_ap_t::iterator> its =
            archivecache_by_archivepart.equal_range(e->archivepart);
        for (ac_by_ap_t::iterator it = its.first; it != its.second; it++)
            if (it->second->filename == filename) { archivecache_by_archivepart.erase(it); break; }
        // see what I mean?
        // taps
        delete e;

        connstamp (clog, connection) << "Retiring archive " << filename << ": "
                                     << pmErrStr_r (rc, pmmsg, sizeof (pmmsg))
                                     << endl;
        return;
    } else if (st.st_mtime == e->metadata_mtime) {
        ; // metrics cache still good
    } else { // need to (re)load the metrics
        e->metadata_mtime = st.st_mtime;

        // open a context if not already open from the new-archive case above
        if (ctx < 0) {
            ctx = pmNewContext (PM_CONTEXT_ARCHIVE, filename.c_str ());
            if (ctx < 0) {
                connstamp (cerr, connection) << "cannot open " << filename << ": "
                                             << pmErrStr_r (ctx, pmmsg, sizeof (pmmsg))
                                             << endl;
                return;
            }
        }

        e->metrics.clear();
        // Expose a pseudo-metric consisting of just the archive name
        e->metrics.push_back(metric_string("_"));
        pmg_enum_context c;
        c.output = & e->metrics;
        (void) pmTraversePMNS_r ("", &pmg_enumerate_pmns, &c);

        if (verbosity > 3)
            connstamp (clog, connection) << "enumerated " << e->metrics.size() << " metrics"
                                         << " in " << e->filename << endl;

        e->last_refresh_time = now;
    }


    // Check freshness of archive end-time.  This will change with any
    // sort of write to the active volume file.  We don't want to pay
    // for opening the context and actually looking for the
    // pmGetArchiveEnd though unless it's still alive (modified more
    // recently than last time we checked).
    //
    // A complication is that archive file volumes can be compressed.
    // If they don't exist at all (but the archive does), we infer
    // that the file must be compressed, and therefore dead, and
    // therefore the initial scan's archive-end query must still be
    // current & accurate.
    //

    string archive_basename = e->filename.substr(0, e->filename.size() - 5);
    char lastvol_name[MAXPATHLEN];
    snprintf(lastvol_name, MAXPATHLEN, "%s.%d", archive_basename.c_str(), e->archive_lastvol_idx);
    char nextvol_name[MAXPATHLEN];
    snprintf(nextvol_name, MAXPATHLEN, "%s.%d", archive_basename.c_str(), e->archive_lastvol_idx+1);
    // NB: see why fstat()ing compressed archives wouldn't work?  Because we don't
    // know their actual file names.
    // NB: C++11 to_string() would help.
    rc = stat(lastvol_name, &st);
    if (rc < 0) {
        // assume the volume is compressed -- or the archive has disappeared
        // say nothing
    } else if (e->archive_lastvol_mtime != 0 && // cached mtim exists
               e->archive_lastvol_mtime == st.st_mtime && // matching cached mtim
               access (nextvol_name, R_OK) != 0) { // no next volume
        // nothing to do
    } else {
        // open a context if not already open from the new-archive or metrics case above
        if (ctx < 0) {
            ctx = pmNewContext (PM_CONTEXT_ARCHIVE, filename.c_str ());
            if (ctx < 0) {
                // poison it
                e->archive_end.tv_sec = 0;
                e->archive_end.tv_usec = 0;
                e->archive_lastvol_mtime = now;
                connstamp (cerr, connection) << "cannot open " << filename << ": "
                                             << pmErrStr_r (ctx, pmmsg, sizeof (pmmsg))
                                             << endl;
                return;
            }
        }

        rc = pmGetArchiveEnd(&e->archive_end);
        if (rc < 0) {
            // poison it
            e->archive_end.tv_sec = 0;
            e->archive_end.tv_usec = 0;
            e->archive_lastvol_mtime = now;
            connstamp (cerr, connection) << "cannot get archive end " << filename << ": "
                                         << pmErrStr_r (rc, pmmsg, sizeof (pmmsg))
                                         << endl;
        }

        // see if we have flopped over to the next volume
        if (access (nextvol_name, R_OK) == 0) {
            // assume we only flopped over by one (otherwise this will
            // trigger again at next refresh)
            e->archive_lastvol_idx ++;
            rc = stat (nextvol_name, &st);
            if (rc < 0) {
                // whoops, we can't stat the new volume??
                connstamp (cerr, connection) << "cannot stat new volume " << nextvol_name << ": "
                                             << pmErrStr_r (rc, pmmsg, sizeof (pmmsg))
                                             << endl;
            }
        }

        // update the cached mtim, whether it's the previous or next volume's stat
        e->archive_lastvol_mtime = st.st_mtime;

        e->last_refresh_time = now;
    }

    if (ctx >= 0)
        pmDestroyContext (ctx);

    if (verbosity > 2)
        connstamp (clog, connection) << "searched " << e->filename
                                     << " (as " << e->archivepart << ")"
                                     << " number of metrics: " << e->metrics.size()
                                     << endl;
}



// Refresh our archivecache database.  This is much harder than it
// sounds, because one thing we must not do is rescan archivesdir
// completely every time, and reopen each .meta archive we find in
// there.  This is called for *every pmgraphite request*, and must be
// quick.
//
// OTOH, if we don't refresh, we'll "just" serve up stale data
// (probably -no- data for some time intervals, where we didn't
// know an archive spanned, or a former archive got renamed/merged)
// or something like that.

void
ac_refresh_all(struct MHD_Connection* connection /* may be null */)
{
    
    // for progress messages
    unsigned num_archives = 0;
    unsigned num_directories = 0;
    time_t last_report = time(NULL);
    time_t first_report = last_report;
    
    const time_t min_refresh_interval = 60; 
    static time_t last_refresh = 0;
    // Don't scan more than once per this long; so we may miss the
    // creation of new archives for that long.
    if (last_refresh > 0 && (last_refresh + min_refresh_interval) >= last_report)
        return;
    last_refresh = last_report;

    if (verbosity > 2)
        connstamp (clog, connection) << "Searching for archives under " << archivesdir << endl;

    // Phase 1: Rescan directories.
    //
    // Scan for all .meta files under the -A directory.  Refresh them.
    //
    // NB: Not scanning all directories & archives fully would require
    // separately tracking -all- directories (including intermediate
    // ones), looking for changes in directory mtime.  We could infer
    // from no mtime-change that there are no new or renamed or
    // removed files in the directory, but nothing about
    // subdirectories.  The freshness-age based algorithm makes
    // routine refreshing of older archives so lightweight that such
    // further optimization may not be needed.
    //
    // XXX: investigate *notify linux apis instead of active scanning.
    
    set<string> refreshed_archivenames;
#if HAVE_FTS_H
    // fts(3) is not available everywhere, and convenient substitutes don't
    // seem to exist either.  nftw(3) is not multithread-safe nor can it operate
    // without global variables; scandir(3) may or may not be defined with the
    // proper _XOPEN_SOURCE rune, and then we have to recurse manually anyway ...
    // maybe just back down to readdir() someday? :-(
    //
    // In the mean time, platforms without fts(3) will get only crippled graphite
    // support, since no archives will be discovered.

    // XXX: parallelization opportunity
    char *fts_argv[2];
    fts_argv[0] = (char *) archivesdir.c_str ();
    fts_argv[1] = NULL;
    FTS *f = fts_open (fts_argv, (FTS_NOCHDIR |
                                  FTS_LOGICAL /* resolve symlinks */ |
                                  FTS_NOSTAT /* don't care about mtime etc. */), NULL);
    if (f == NULL) {
        connstamp (cerr, connection) << "cannot fts_open " << archivesdir << endl;
    } else {
        for (FTSENT * ent = fts_read (f); ent != NULL; ent = fts_read (f)) {
            if (exit_p)
                break; // don't bypass the fts_close()
        
            switch(ent->fts_info) {
            case FTS_F:
                if (has_suffix (ent->fts_path, ".meta")) {
                    num_archives ++;
                    string archivename = string(ent->fts_path);
                    refreshed_archivenames.insert(archivename);
                    ac_refresh (connection, archivename);
                }
                break;
            case FTS_D:
                num_directories ++;
                break;
            default:
                // do nothing with files etc.
                ;
            }
        
            // Update the user about our progress, as this can take time.
            if (verbosity > 1) {
                time_t now = time(NULL);
                if (now >= last_report+2) { // i.e., two seconds have elapsed
                    last_report = now;
                    connstamp (clog, connection) << "Refreshing... directories: " << num_directories
                                                 << " archives: " << num_archives
                                                 << endl;
                }
            }
        }
        fts_close (f);
    }
#endif

    // Phase 2: rescan other archives in the archivecache
    //
    // In the process, some of them may remove themselves (if the
    // underlying archive has disappeared).
    //
    // XXX: optimize: it'd be nice avoid refreshing those archives
    // that are certainly unrelated to the current web query (e.g.,
    // mismatching hostname).
    //
    // XXX: optimize: it's not urgent to refresh (nuke) dead archives
    // We could do this scan very rarely.
    //
    // NB: we do want to clean up fully at exit_p, for valgrind hygiene
    
    set<string> archivenames;
    for (ac_by_fn_t::iterator it = archivecache_by_filename.begin();
         it != archivecache_by_filename.end();
         it = archivecache_by_filename.upper_bound(it->first)) {
        archivenames.insert(it->first);
    }
    for (set<string>::iterator it = archivenames.begin();
         it != archivenames.end();
         it ++) {
        const string& archivename = *it;
        if (refreshed_archivenames.find(archivename) != refreshed_archivenames.end())
            continue; // no need to do it again
        
        num_archives ++;
        ac_refresh (connection, archivename);
    }

    // one final report if we took a long time
    if (verbosity > 1) {
        time_t now = time(NULL);
        if (now >= first_report+2) { // i.e., two seconds have elapsed
            connstamp (clog, connection) << "Processed " << num_archives << " archives and "
                                         << num_directories << " directories total "
                                         << "under " << archivesdir
                                         << " in " << (now - first_report) << " seconds"
                                         << endl;
        }
    }

    // print out a dumpstats-periodic report of our archivecache
    static time_t last_dumpstats = 0;
    if (dumpstats > 0 && (last_dumpstats + dumpstats) < first_report) {
        timestamp (clog) << "Archive cache: "
                         << archivecache_by_filename.size() << " files, "
                         << archivecache_by_archivepart.size() << " names" << endl;
        last_dumpstats = time(NULL);
    }
    
}



vector<metric_string> pmgraphite_enumerate_metrics (struct MHD_Connection * connection,
                                                    const vector<string> & patterns_tok,
                                                    time_t t_start = 0,
                                                    time_t t_end = std::numeric_limits<time_t>::max())
{
    vector<metric_string> output;

    // Freshen up.
    ac_refresh_all(connection);

    // OK, now the archivecache has all the metrics from all the
    // archives.  Time to collect those that satisfy the incoming
    // query.  We need to search apprx. the whole cache here, because
    // the archivecache_by_archivepart can't help accelerate globby
    // searches.
    for (ac_by_ap_t::iterator it = archivecache_by_archivepart.begin();
         it != archivecache_by_archivepart.end();
         it++) {
        const archivecache_entry *e = it->second;

        // filter archivepart mismatches
        if (patterns_tok.size()>0 &&
            fnmatch (patterns_tok[0].c_str(), e->archivepart.c_str(), FNM_NOESCAPE))
            continue;
        // Reject out-of-bounds time
        if (e->archive_end.tv_sec < t_start)
            continue;
        if (e->archive_begin.tv_sec > t_end)
            continue;

        // filter metric mismatches
        for (unsigned i = 0;
             i != e->metrics.size();
             i++) {
            const metric_string& metricpart = e->metrics[i];
            bool match = true;
            const vector<string>& metric_parts = metricpart.split();
            for (unsigned i = 0; i < metric_parts.size (); i++) {
                if (patterns_tok.size () > i + 1) {
                    // patterns[0] was already used for the archive name
                    const string& metricpart = metric_parts[i];
                    const string& pattern = patterns_tok[i + 1];
                    if (fnmatch (pattern.c_str (), metricpart.c_str (), FNM_NOESCAPE) != 0) {
                        match = false;
                        break;
                    }
                }
            }
            if (match)
                output.push_back(metric_string(e->archivepart, metricpart));
        }
    }

    if (verbosity > 2) {
        connstamp (clog, connection) << "enumerated " << output.size () << " metrics" << endl;
    }

    // NB: not necessary to sort(), since set<>s are already sorted
    sort (output.begin(), output.end());
    output.erase (unique (output.begin(), output.end()), output.end());
    return output;
}


vector<metric_string> pmgraphite_enumerate_metrics (struct MHD_Connection * connection,
                                                    const string & pattern,
                                                    time_t t_start = 0,
                                                    time_t t_end = std::numeric_limits<time_t>::max())
{
    vector<string> pattern_tok = split(pattern, '.');
    return pmgraphite_enumerate_metrics (connection, pattern_tok, t_start, t_end);
}




// ------------------------------------------------------------------------


// This query traverses the metric tree one level at a time.  Incoming queries
// look like "foo.*", and we're expected to return "foo.bar", "foo.baz", etc.,
// differentiating leaf nodes from subtrees.
// We also handle the "format=completer" option.

int
pmgraphite_respond_metrics_find (struct MHD_Connection *connection,
                                 const http_params & params, const vector <string> &/*url*/)
{
    int rc;
    struct MHD_Response *resp;

    string query = params["query"];
    string format = params["format"];
    if (format == "") {
        format = "treejson";    // as per grafana
    }

    vector <string> query_tok = split (query, '.');
    assert (query_tok.size() >= 1);
    // suffix last query component with '*'
    string& last_tok = query_tok[query_tok.size()-1];
    if (last_tok.empty() || last_tok[last_tok.size()-1] != '*')
        last_tok += string("*");

    vector<metric_string> metrics = pmgraphite_enumerate_metrics (connection, query_tok);
    if (exit_p)
        return MHD_NO;

    // The metrics<> vector contains all possible full metric strings
    // for the query_tok prefix.   We need to transform this into a one-step
    // expansion - just those components that match the query_tok<> prefix,
    // stripping the suffixes.

    unsigned prefix_size = query_tok.size ();

    // these sets are used for duplicate-elimination
    map <string,bool> metric_leaf;   // foo.bar -> true (leaf) or false (has .baz/.zoo descendants)
    map <string,string> metric_last; // foo.bar -> bar (for response JSON name field)
    for (mvi_t it = metrics.begin(); it != metrics.end(); it++) {
        if (exit_p)
            return MHD_NO;

        const vector<string>& pieces = it->split();
        if (pieces.size () < prefix_size)
            continue;		// should not happen

        // trim it to preserve just the first N (requested) levels only, do not expand!
        string prefix;
        for (unsigned j=0; j<prefix_size; j++) {
            if (j>0)
                prefix += string(".");
            prefix += pieces[j];
        }

        // NB: due to properties of the PMNS, we won't have a metric
        // prefix be both a leaf and non-leaf, because we can't have a
        // PCP metrics named foo.bar AND foo.bar.baz.
        if (pieces.size () > prefix_size)
            metric_leaf [prefix] = false;
        if (pieces.size() == prefix_size)
            metric_leaf [prefix] = true;

        metric_last[prefix] = pieces[prefix_size-1];
    }

    // OK, time to generate some output.
    stringstream output;
    unsigned num_nodes = 0;

    if (format == "completer") {
        output << "{ \"metrics\": ";
    }

    output << "[";
    for (map <string,bool>::iterator it = metric_leaf.begin (); it != metric_leaf.end ();
            it ++) {
        const string & prefix = it->first;
        bool leaf_p = it->second;
        const string & name = metric_last[prefix];

        if (num_nodes++ > 0) {
            output << ",";
        }

        if (format == "completer") {
            output << "{";
            json_key_value (output, "name", name, ",");
            if (leaf_p)
                json_key_value (output, "path", prefix, ",");
            else
                json_key_value (output, "path", (string) (prefix + string(".")), ",");
            json_key_value (output, "is_leaf", leaf_p);
            output << "}";
        } else {
            output << "{";
            json_key_value (output, "text", name, ",");
            json_key_value (output, "id", prefix, ",");
            json_key_value (output, "leaf", leaf_p, ",");
            json_key_value (output, "expandable", !leaf_p, ",");
            json_key_value (output, "allowChildren", !leaf_p);
            output << "}";
        }
    }
    output << "]";

    if (format == "completer") {
        output << "}";
    }


    // wrap it up in mhd response ribbons
    resp = NOTMHD_compressible_response (connection, output.str());
    if (resp == NULL) {
        connstamp (cerr, connection) << "MHD_create_response_from_buffer failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    /* https://developer.mozilla.org/en-US/docs/HTTP/Access_control_CORS */
    rc = MHD_add_response_header (resp, "Access-Control-Allow-Origin", "*");
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_add_response_header ACAO failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    rc = MHD_add_response_header (resp, "Content-Type", "application/json");
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_add_response_header CT failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }
    rc = MHD_queue_response (connection, MHD_HTTP_OK, resp);
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_queue_response failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    MHD_destroy_response (resp);
    return MHD_YES;

out1:
    return mhd_notify_error (connection, rc);
}



// ------------------------------------------------------------------------


// This query searches the whole metric list for substring / regexp matches.
// Unusually, this request returns comma-separated results as text/plain (?!).
// The graphlot flavour is almost identical.

int
pmgraphite_respond_metrics_grep (struct MHD_Connection *connection,
                                 const http_params & params,
                                 const vector <string> &/*url*/,
                                 bool graphlot_p)
{
    int rc;
    struct MHD_Response *resp;

    string query = graphlot_p ? params["q"] : params["query"];
    if (query == "") {
        return mhd_notify_error (connection, -EINVAL);
    }

    vector <string> query_tok = split (query, ' ');
    vector <regex_t> query_regex;

    for (unsigned i=0; i<query_tok.size (); i++) {
        regex_t re;
        rc = regcomp (&re, query_tok[i].c_str (), REG_ICASE | REG_NOSUB);
        if (rc == 0) {
            query_regex.push_back (re);    // copied by value
        }
    }

    vector<metric_string> metrics = pmgraphite_enumerate_metrics (connection, "*");
    if (exit_p) {
        return MHD_NO;
    }

    stringstream output;
    unsigned count = 0;

    for (mvi_t it = metrics.begin(); it != metrics.end(); it++) {
        const metric_string& m = *it;
        const string mstr = m.unsplit();
        bool result = true;
        for (unsigned j=0; j<query_regex.size (); j++) {
            rc = regexec (& query_regex[j], mstr.c_str (), 0, NULL, 0);
            if (rc != 0) {
                result = false;
            }
        }
        if (result) {
            if (graphlot_p) {
                output << mstr << endl;
            } else {
                if (count++ > 0)
                    output << ",";
                output << mstr;
            }
        }
    }

    // wrap it up in mhd response ribbons
    resp = NOTMHD_compressible_response (connection, output.str());
    if (resp == NULL) {
        connstamp (cerr, connection) << "MHD_create_response_from_buffer failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    /* https://developer.mozilla.org/en-US/docs/HTTP/Access_control_CORS */
    rc = MHD_add_response_header (resp, "Access-Control-Allow-Origin", "*");
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_add_response_header ACAO failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    rc = MHD_add_response_header (resp, "Content-Type", "text/plain");
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_add_response_header CT failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }
    rc = MHD_queue_response (connection, MHD_HTTP_OK, resp);
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_queue_response failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    MHD_destroy_response (resp);
    return MHD_YES;

out1:
    return mhd_notify_error (connection, rc);
}


// ------------------------------------------------------------------------


// A pre-scaled, pre-initialized vector of floats

struct timeseries {
    time_t t_start, t_end, t_step; // inclusive limits
    vector<float> data;

    timeseries(time_t a, time_t b, time_t c):
        t_start(a), t_end(b), t_step(c),
        data((size_t) (b-a)/c+1, nanf(""))
    {
    }

    const float& at(time_t t) const
    {
        assert(t >= t_start && t <= t_end);
        return data[(t-t_start)/t_step];
    }
    float& at(time_t t)
    {
        assert(t >= t_start && t <= t_end);
        return data[(t-t_start)/t_step];
    }

    // for indexed access
    size_t size() const { return data.size(); }
    time_t when(unsigned i) const { return t_start + t_step*i; }
    const float& operator [](unsigned i) const
    {
        assert(i <= data.size());
        return data[i];
    }
    float& operator [](unsigned i)
    {
        assert(i <= data.size());
        return data[i];
    }
};


// Rate-convert this timeseries, because it came from a PM_SEM_COUNTER
// metric.  Leave NaN's where rate conversion is impossible due to gaps
// or missing values.  Modifies incoming
void timeseries_rateconvert(timeseries& ts)
{
    vector<float> rated_data (ts.data.size());

    for (unsigned i=0; i<ts.size(); i++) {
        float this_value = ts.data[i];
        float last_value = (i>0) ? ts.data[i-1] : nanf("");
        float result;

        if (! isfinite(this_value))
            result = this_value;
        else if (! isfinite(last_value))
            result = last_value;
        else if (this_value < last_value) // suspect counter overflow
            result = nanf("");
        else {
            // rate-convert to per-second basis
            // truncate time at seconds; we can't accurately subtract two large integers
            // when represented as floating point anyways
            time_t delta = ts.t_step;
            assert (delta > 0);

            // avoid loss of significance risk of naively calculating
            // (double)(this_v-last_v)/(double)(this_t-last_t)
            result = (this_value / delta) - (last_value / delta);
        }

        // NB: we write to a temporary array, because if we update data[i],
        // then the rate calculation for data[i+1] would have the wrong input.
        rated_data[i] = result;
    }

    // copy them over
    ts.data = rated_data;
}

// Convert all NaNs to given value
void timeseries_nullconvert(timeseries& ts, float v)
{
    for (unsigned i=0; i<ts.size(); i++)
        if (! isfinite(ts.data[i]))
            ts.data[i] = v;
}


// parameters for fetching a series
struct fetch_series_jobspec {
#ifdef HAVE_PTHREAD_H
    vector<pthread_mutex_t*> output_locks; // protection for the individual 'rows' of output
#endif
    vector<timeseries*> outputs;
    vector<pmDesc*> output_descs;

    vector<metric_string> targets;
    string filename; // archive filename
    time_t t_start, t_end, t_step;
    string message; // may have error or verbose message
};


template <class Spec>
struct fetch_series_jobqueue {
#ifdef HAVE_PTHREAD_H
    pthread_mutex_t lock; // protects following field
#endif
    unsigned next_job_nr;

    vector<Spec> jobs; // vector itself read-only
    typedef void (*runner_t) (Spec *);
    runner_t runner;

    fetch_series_jobqueue (runner_t);
    ~fetch_series_jobqueue ();

    static void* thread_main (void *);

    void run ();
};


template <class Spec>
fetch_series_jobqueue<Spec>::fetch_series_jobqueue (runner_t r)
{
#ifdef HAVE_PTHREAD_H
    pthread_mutex_init (& this->lock, NULL);
#endif
    this->runner = r;
    this->next_job_nr = 0;
}


template <class Spec>
fetch_series_jobqueue<Spec>::~fetch_series_jobqueue ()
{
#ifdef HAVE_PTHREAD_H
    pthread_mutex_destroy (& this->lock);
#endif
}


template <class Spec>
void* fetch_series_jobqueue<Spec>::thread_main (void *cls)
{
    fetch_series_jobqueue<Spec>* q = (fetch_series_jobqueue<Spec>*) cls;
    assert (q != 0);

    while (1) {
#ifdef HAVE_PTHREAD_H
        pthread_mutex_lock (& q->lock);
#endif
        unsigned my_jobid = q->next_job_nr++;
#ifdef HAVE_PTHREAD_H
        pthread_mutex_unlock (& q->lock);
#endif

        if ((my_jobid >= q->jobs.size ()) || exit_p) {
            break;
        }

        (*q->runner) (& q->jobs[my_jobid]);
    }
    return 0;
}



// simple contention
template <class Spec>
void fetch_series_jobqueue<Spec>::run ()
{
    // Permute the job queue randomly, to make it less likely that
    // concurrent threads are processing the same archive.
    // ... but some experimentaton shows harm rather than benefit.
    // random_shuffle (this->jobs.begin(), this->jobs.end());
    // ... plus we'd have to unshuffle before results are collected.

#ifdef HAVE_PTHREAD_H
    vector<pthread_t> threads;
    for (unsigned i=0; i<multithread; i++) {
        pthread_t x;
        int rc = pthread_create (&x, NULL, & this->thread_main, (void*) this);
        if (rc == 0) {
            threads.push_back (x);
        }
    }
#endif
    // have main thread also have a go
    (void)this->thread_main (this);

#ifdef HAVE_PTHREAD_H
    for (unsigned i=0; i<threads.size (); i++) {
        (void) pthread_join (threads[i], NULL);
    }
#endif
}



// Heavy lifter.  Parse graphite "target" name into archive
// file/directory, metric names, and (if appropriate) instances within
// metric indom; fetch all the data values interpolated between given
// inclusive-end time points, and assemble them into given vector of
// series-of-numbers for rendering.
//
// A lot can go wrong, but is signalled only with a stderr message and
// an empty vector.  (As a matter of security, we prefer not to give too
// much information to a remote web user about the exact error.)  Occasional
// missing metric values are represented as floating-point NaN values.
void pmgraphite_fetch_series (fetch_series_jobspec *spec)
{
    assert (spec != NULL);
    assert (spec->outputs.size() > 0);
    assert (spec->targets.size() == spec->outputs.size());

    // vectors are indexed parallel with spec->targets[] == spec->outputs[]
    time_t t_start = spec->t_start;
    time_t t_end = spec->t_end;
    time_t t_step = spec->t_step;
    int sts;
    string last_component;
    int pmc;
    string archive;
    unsigned entries_good = 0, entries;
    stringstream message;
    pmLogLabel archive_label;
    struct timeval archive_end;
    int pmSetMode_called_p = 0;
    vector<pmID> pmids;
    vector<pmDesc> pmdescs;
    vector<int> pminsts;

    set<pmID> pmids_set;
    vector<pmID> unique_pmids;

    // ^^^ several of these declarations are here (instead of at
    // point-of-use) only because we jump to an exit point, and may
    // not leap over an object ctor site.

    // XXX: in future, parse graphite functions-of-metrics
    // http://graphite.readthedocs.org/en/latest/functions.html
    //
    // XXX: in future, cache the pmid/pmdesc/inst# -> pcp-context

    // -------------------- PART 1 - per-archive processing

    archive = spec->filename;

    // Open the bad boy.
    pmc = pmNewContext (PM_CONTEXT_ARCHIVE, archive.c_str ());
    if (pmc < 0) {
        // error already noted XXX where?
        goto out0;
    }

    // NB: past this point, exit via 'goto out;' to release pmc

    // Fetch end of archive time boundaries, to avoid having libpcp
    // iterate across vast regions of void.  This would be especially
    // bad if libpcp worries the archive might have grown since last
    // call, go and do an fstat(2)/lseek(2) every point.
    sts = pmGetArchiveLabel (& archive_label);
    if (sts < 0) {
        message << "cannot find archive label";
        goto out;
    }

    sts = pmGetArchiveEnd (& archive_end);
    if (sts < 0) {
        message << "cannot find archive end";
        goto out;
    }

    if (verbosity > 3) {
        message << "[" << archive_label.ll_start.tv_sec
                << "-" << archive_end.tv_sec << "] ";
    }
    // XXX ^^^ redundant with archivecache


    // -------------------- PART 2 - per-metric metadata processing

    pmids.resize(spec->targets.size());
    pmdescs.resize(spec->targets.size());
    pminsts.resize(spec->targets.size());

    for (unsigned j=0; j<spec->targets.size(); j++) {
        if (exit_p)
            break;

        const metric_string& target = spec->targets[j];
        pmids[j] = 0; // always invalid

        const vector<string>& target_tok = target.split();
        if (target_tok.size () < 2) {
            message << " " << target.unsplit() << ": not enough target components";
            continue;
        }
        for (unsigned i = 0; i < target_tok.size (); i++)
            if (target_tok[i] == "") {
                message << " " << target.unsplit() << ": empty target components";
                continue;
            }

        // We need to decide whether the next dotted components represent
        // a metric name, or whether there is an instance name squished at
        // the end.
        string metric_name = "";
        for (unsigned i = 1; i < target_tok.size () - 1; i++) {
            const string & piece = target_tok[i];
            if (i > 1) {
                metric_name += '.';
            }
            metric_name += piece;
        }
        last_component = target_tok[target_tok.size () - 1];

        char *namelist[1];
        pmID pmidlist[1]; // fetch here instead of pmids[i], so an early error continue leaves latter zero
        namelist[0] = (char *) metric_name.c_str ();
        int sts = pmLookupName (1, namelist, & pmidlist[0]);

        if (sts == 1) {
            // found ... last name must be instance domain name
            sts = pmLookupDesc (pmidlist[0], &pmdescs[j]);
            if (sts != 0) {
                if (! graphite_hostcache) // this is normal in -J mode; mixing archives
                    message << " cannot find metric descriptor " << metric_name;
                continue;
            }
            // check that there is an instance domain, in order to use that last component
            if (pmdescs[j].indom == PM_INDOM_NULL) {
                if (! graphite_hostcache) // this is normal in -J mode; mixing archives
                    message << " metric " << metric_name << " lacks expected indom "
                            << last_component;
                continue;
            }
            // look up that instance name
            string instance_name = pmgraphite_metric_decode (last_component);
            int inst = pmLookupInDomArchive (pmdescs[j].indom,
                                             (char *) instance_name.c_str ());
            if (inst < 0) {
                if (! graphite_hostcache) // this is normal in -J mode; mixing archives
                    message << " metric " << metric_name << " lacks recognized indom "
                            << last_component;
                continue;
            }
            pminsts[j] = inst;
            // NB: don't mess with instance domain profiles.  We may have multiple
            // contradictory sets for different metrics in the same fetch loop.
            // Instead we receive them all and search through them via pminst[i].
        } else {
            // not found ... ok, try again with that last component
            metric_name = metric_name + '.' + last_component;
            namelist[0] = (char *) metric_name.c_str ();
            int sts = pmLookupName (1, namelist, pmidlist);
            if (sts != 1) {
                // still not found .. give up
                if (! graphite_hostcache) // this is normal in -J mode; mixing archives
                    message << " cannot find metric name " << metric_name;
                continue;
            }

            sts = pmLookupDesc (pmidlist[0], &pmdescs[j]);
            if (sts != 0) {
                message << " cannot find metric descriptor " << metric_name;
                continue;
            }
            // check that there is no instance domain
            if (pmdescs[j].indom != PM_INDOM_NULL) {
                message << " metric " << metric_name << " has unexpected indom " << pmdescs[j].indom;
                continue;
            }

            pminsts[j] = -1; // PMAPI magic value for pmResult inst for PM_INDOM_NULL
        }

        // Check that the pmDesc type is numeric
        switch (pmdescs[j].type) {
        case PM_TYPE_32:
        case PM_TYPE_U32:
        case PM_TYPE_64:
        case PM_TYPE_U64:
        case PM_TYPE_FLOAT:
        case PM_TYPE_DOUBLE:
            break;
        default:
            message << " metric " << metric_name << " has unsupported type " << pmdescs[j].type;
            continue;
        }

        pmids[j] = pmidlist[0]; // Now we're committed to trying to fetch this pmid.

        // supply the pmDesc to caller
#ifdef HAVE_PTHREAD_H
        pthread_mutex_lock (spec->output_locks[j]);
#endif
        *(spec->output_descs[j]) = pmdescs[j];
#ifdef HAVE_PTHREAD_H
        pthread_mutex_unlock (spec->output_locks[j]);
#endif
    }

    // -------------------- PART 3 - giant fetch loop

    // compute unique subset of pmids; we search through pmResult
    pmids_set.insert(pmids.begin(), pmids.end());
    unique_pmids.insert(unique_pmids.begin(), pmids_set.begin(), pmids_set.end());

    // inclusive iteration from t_start to t_end
    pmSetMode_called_p = 0;

    entries = 0; // index in (*outputs[i]) to fill - i.e., a scaled time coordinate
    for (time_t iteration_time = t_start; iteration_time <= t_end; iteration_time += t_step, entries++) {
        if (exit_p)
            break;

        pmResult *result;

        // We only want to pmFetch within known time boundaries of the archive.
        if (iteration_time >= archive_label.ll_start.tv_sec &&
                iteration_time <= archive_end.tv_sec) {

            if (! pmSetMode_called_p) {
                struct timeval start_timeval;
                start_timeval.tv_sec = iteration_time;
                start_timeval.tv_usec = 0;
                sts = pmSetMode (PM_MODE_INTERP | PM_XTB_SET (PM_TIME_SEC), &start_timeval, t_step);
                if (sts != 0) {
                    message << " cannot set time mode origin/delta";
                    break;
                }

                pmSetMode_called_p = 1;
            }

            // fetch all unique metrics
            int sts = pmFetch (unique_pmids.size(), & unique_pmids[0], &result);
            if (verbosity > 4) {
                message << "\n@" << iteration_time; // also == result->timestamp.tv_sec
                if (sts < 0)
                    message << "?";
                message << " ";
            }

            if (sts >= 0) {
                assert ((size_t)result->numpmid == unique_pmids.size()); // PMAPI guarantee?

                // search them all for matching pmid/inst tuples
                for (unsigned i=0; i<spec->targets.size(); i++) {
                    for (unsigned j=0; j<unique_pmids.size(); j++) { // for indexing over result->vset
                        if (result->vset[j]->pmid != pmids[i])
                            continue;

                        for (int k=0; k<result->vset[j]->numval; k++) {
                            if (result->vset[j]->vlist[k].inst != pminsts[i])
                                continue;

                            // yey, found our (pmid,inst) value!!

                            pmAtomValue value;
                            sts = pmExtractValue (result->vset[j]->valfmt,
                                                  &result->vset[j]->vlist[k],	// we know: one pmid, one instance
                                                  pmdescs[i].type, &value, PM_TYPE_FLOAT);
                            if (sts == 0) {
                                // overwrite the pre-prepared NaN with our genuine value
#ifdef HAVE_PTHREAD_H
                                pthread_mutex_lock (spec->output_locks[i]);
#endif
                                (*spec->outputs[i]).at(result->timestamp.tv_sec) = value.f;
#ifdef HAVE_PTHREAD_H
                                pthread_mutex_unlock (spec->output_locks[i]);
#endif
                                if (verbosity > 4)
                                    message << value.f << " ";
                                entries_good++;
                            }

                            j = unique_pmids.size(); // arrange to exit the valuesets iteration too
                            break;
                        } // search over instances
                    } // search over valuesets

                } // done iterating over all targets

                pmFreeResult (result);
            }
        }
    } // iterate over time

    if ((verbosity > 3) || (verbosity > 2 && entries_good > 0)) {
        message << spec->targets.size() << " targets(s) (" << pmids_set.size() << " unique metrics)";
        message << ", " << entries_good << "/" << entries*spec->targets.size() << " values";
    }

 out:
    pmDestroyContext (pmc);
 out0:
    // vector output already returned via jobspec pointer

    spec->message = message.str (); // pass back message
    // ... but prefix it with archive name
    if (spec->message.size() > 0 && // have -some- message
        (entries_good == 0 || verbosity > 2)) // big fetch error or verbosity
        spec->message = archive + ": " + spec->message;
}



// A parallelizable version of the above.

void
pmgraphite_fetch_all_series (struct MHD_Connection* connection,
                             const vector<metric_string>& targets,
                             vector<timeseries>& outputs,
                             vector<pmDesc>& output_descs,
                             time_t t_start, time_t t_end, time_t t_step)
{
    // create some jobspecs, one per archive
    // with many little empty vectors inside
    outputs.resize(targets.size(), timeseries(t_start,t_end,t_step));
    output_descs.resize(targets.size());
    map <string, fetch_series_jobspec> jobmap;
#ifdef HAVE_PTHREAD_H
    vector <pthread_mutex_t> output_locks(targets.size());
    for (unsigned i=0; i<targets.size(); i++)
        pthread_mutex_init(& output_locks[i], NULL);
#endif

    for (unsigned i = 0; i < targets.size (); i++) {
        const metric_string& target = targets[i];
        const vector<string>& target_tok = target.split();

        // We used to reject target_tok.size() < 2 here, because it's
        // missing the metric name.  However, we now support this as a
        // "archive time coverage" pseudo-metric: "_".

        // Map the first component to the set of matching
        // archivecache_entry's, and thence to their archive
        // filenames.

        pair<ac_by_ap_t::iterator,ac_by_ap_t::iterator> entries =
            archivecache_by_archivepart.equal_range(target_tok[0]); // wildcards expanded already
        for (ac_by_ap_t::iterator it = entries.first;
             it != entries.second;
             it++) {
            const archivecache_entry *e = it->second;

            // Already rejected mismatching archivepart
            assert (target_tok[0] == e->archivepart);
            // Reject out-of-bounds time
            if (e->archive_end.tv_sec < t_start)
                continue;
            if (e->archive_begin.tv_sec > t_end)
                continue;

            // handle ._ pseudo-metric here, so we don't even have to spin up
            // threads & risk pmNewContext-opening archives, just to enumerate
            if (target_tok[1] == "_" && target_tok.size() == 2) {
                // invent a pmDesc - but only the parts used
                output_descs[i].sem = PM_SEM_INSTANT;
                output_descs[i].units.scaleSpace = 0;
                output_descs[i].units.scaleTime = 0;
                output_descs[i].units.scaleCount = 0;
                output_descs[i].units.dimSpace = 0;
                output_descs[i].units.dimTime = 0;
                output_descs[i].units.dimCount = 1;

                // set to 0 the time interval that overlaps the archive's timespan and the query timespan
                for (time_t w = max(t_start,e->archive_begin.tv_sec);
                     w <= min(t_end,e->archive_end.tv_sec);
                     w += t_step)
                    (outputs[i]).at(w) = 0;
                
                // skip rest of jobmap etc. processing
                continue;
            }
            
            map<string,fetch_series_jobspec>::iterator it2 = jobmap.find(e->filename);
            if (it2 == jobmap.end()) {
                fetch_series_jobspec js;
                js.t_start = t_start;
                js.t_end = t_end;
                js.t_step = t_step;
                js.filename = e->filename;
                it2 = jobmap.insert(make_pair(e->filename,js)).first;
            }

            it2->second.targets.push_back (target);
#ifdef HAVE_PTHREAD_H
            it2->second.output_locks.push_back (& output_locks[i]);
#endif
            it2->second.outputs.push_back (& outputs[i]);
            it2->second.output_descs.push_back (& output_descs[i]);
        }
    }

    // copy into a jobqueue vector (since the execution loop wants a vector)
    fetch_series_jobqueue<fetch_series_jobspec> q (& pmgraphite_fetch_series);
    for (map<string,fetch_series_jobspec>::iterator it = jobmap.begin(); it != jobmap.end(); it++)
        q.jobs.push_back(it->second);

    unsigned number_of_jobs = q.jobs.size();

    // it's ready to go
    struct timeval start;
    (void) gettimeofday (&start, NULL);
    q.run ();
    struct timeval finish;
    (void) gettimeofday (&finish, NULL);
    // ... aaaand it's gone

#ifdef HAVE_PTHREAD_H
    for (unsigned i=0; i<targets.size(); i++)
        pthread_mutex_destroy(& output_locks[i]);
#endif

    // propagate any messages
    for (unsigned i = 0; i < q.jobs.size (); i++) {
        const string& message = q.jobs[i].message;
        if (message != "") {
            connstamp (clog, connection) << message << endl;
        }
    }

    // rate conversion
    // NB: this is where we could deal with other functiosn
    for (unsigned i=0; i<targets.size(); i++)
        if (output_descs[i].sem == PM_SEM_COUNTER)
            timeseries_rateconvert(outputs[i]);

    if (verbosity > 1) {
        connstamp (clog, connection) << "digested " << targets.size () << " metric(s)"
                                     << " over " << number_of_jobs << " archive(s)"
                                     << ", timespan [" << t_start << "-" << t_end
                                     << " by " << t_step << "]"
                                     << ", in " << pmtimevalSub (&finish,&start)*1000 << "ms "
                                     << endl;
    }

    assert (outputs.size () == targets.size ());
}


/* ------------------------------------------------------------------------ */


// Attempt to parse a graphite time-specification value, such as the parameter
// to the /graphite/rawdata/from=*&until=* parameters.  Negative values are
// relative to "now" (=> set relative_p); non-negative values
// are considered absolute (=> leave relative_p alone).
//
// The exact syntax permitted is tricky.  It's only partially documented
// http://graphite.readthedocs.org/en/latest/render_api.html#data-display-formats
// whereas implementation in graphite/render/attime.py is concrete but too much
// to duplicate here.
//
// Return the absolute seconds value, or "now" in case of error.

time_t
pmgraphite_parse_timespec (struct MHD_Connection *connection, string value, int &relative_p)
{
    // just delegate to __pmParseTime()
    struct timeval now;
    (void) gettimeofday (&now, NULL);
    struct timeval result;
    char *errmsg = NULL;
    char *end;
    struct tm parsed;

    if (value == "") {
        connstamp (cerr, connection) << "empty graphite timespec" << endl;
        return now.tv_sec;
    }

    // detect the EPOCH absolute-time format emitted by grafana >1.6
    memset (&parsed, 0, sizeof (parsed));
    end = strptime (value.c_str (), "%s", &parsed);
    if (end != NULL && *end == '\0') {
        // success
        return mktime (&parsed);
    }

    // detect the HH:MM_YYYYMMDD absolute-time format emitted by grafana
    memset (&parsed, 0, sizeof (parsed));
    end = strptime (value.c_str (), "%H:%M_%Y%m%d", &parsed);
    if (end != NULL && *end == '\0') {
        // success
        return mktime (&parsed);
    }

    // We can't unambiguously detect whether the input string is
    // relative or not; __pmParseTime doesn't tell us whether 'now'
    // was mentioned, for example.  So we presume relativeness.
    relative_p = 1;

    // don't parse YYYYMMDD, since it's not syntactically separable from EPOCH

    if (value[0] != '-') {
        // nonnegative?  lead __pmParseTime to interpret it as absolute
        // (this is why we take string value instead of const string& parameter)
        value = string ("@") + value;
    }

    // XXX: graphite permits time units of "weeks", "months", "years",
    // even though the latter two can't refer to a fixed number of seconds.
    // That's OK, heuristics and approximations are acceptable; __pmParseTime()
    // should probably learn to accept them.
    int sts = __pmParseTime (value.c_str (), &now, &now, &result, &errmsg);
    if (sts != 0) {
        connstamp (cerr, connection) << "unparseable graphite timespec " << value << ": " <<
                                     errmsg << endl;
        free (errmsg);
        return now.tv_sec;
    }

    return result.tv_sec;
}


/* ------------------------------------------------------------------------ */

// Decode graphite URL pieces toward data gathering: specifically enough
// to identify validated metrics and time bounds.  Set t_relative_p if
// any time coordinates were relative.

int
pmgraphite_gather_data (struct MHD_Connection *connection,
                        const http_params & params,
                        const vector <string> &/*url*/,
                        vector<metric_string>& targets,
                        time_t& t_start,
                        time_t& t_end,
                        time_t& t_step,
                        int &t_relative_p)
{
    int rc = 0;

    vector<string> target_patterns = params.find_all ("target");

    // same defaults as python graphite/graphlot/views.py
    string from = params["from"];
    if (from == "") {
        from = "-24hour";
    }
    string until = params["until"];
    if (until == "") {
        until = "-0hour";
    }

    t_relative_p = 0;
    t_start = pmgraphite_parse_timespec (connection, from, t_relative_p);
    t_end = pmgraphite_parse_timespec (connection, until, t_relative_p);

    // sanity-check time interval
    if (t_start >= t_end) {
        return -EINVAL;
    }

    // The patterns may have wildcards; expand the bad boys - and restrict to the start/end times
    for (unsigned i=0; i<target_patterns.size (); i++) {
        unsigned pattern_length = count (target_patterns[i].begin (), target_patterns[i].end (), '.');
        vector<metric_string> metrics = pmgraphite_enumerate_metrics (connection, target_patterns[i], t_start, t_end);
        if (exit_p) {
            break;
        }

        // NB: the entries in enumerated metrics[] may be wider than
        // the incoming pattern, for example for a wildcard like *.*
        // We need to filter out those enumerated ones that are longer
        // (have more dot-components) than the incoming pattern.

        for (mvi_t it = metrics.begin(); it != metrics.end(); it++) {
            if (pattern_length == it->split_size()-1)
                targets.push_back (*it);
            }
    }

    // Compute t_step.  Because we calculate with integers, the
    // minimum is 1.  The practical minimum is something dependent on
    // the archive's sampling rate for this particular metric, since
    // supersampling wastes CPU.
    int maxdatapt = atoi (params["maxDataPoints"].c_str ());	// ignore failures
    if (maxdatapt <= 0) {
        maxdatapt = 1024;		// a sensible upper limit?
    }

    t_step = graphite_timestep;
    // make it larger if needed; maxdatapt governs
    if (((t_end - t_start) / t_step) > maxdatapt) {
        t_step = ((t_end - t_start) / maxdatapt) + 1;
    }

    return rc;
}


/* ------------------------------------------------------------------------ */


#ifdef HAVE_CAIRO

cairo_status_t notcairo_write_to_string (void *cls, const unsigned char* data, unsigned int length)
{
    string* s = (string *) cls;
    (*s) += string ((const char*) data, (size_t) length); // XXX: might throw
    return CAIRO_STATUS_SUCCESS;
}


// Generate a color wheel, with NUM entries, based on a possibly-shorter colorList.
vector<string> generate_colorlist (const vector<string>& colorList, unsigned num)
{
    vector<string> output;

    for (unsigned i=0; i<num; i++) {
        if (colorList.size () > 0) {
            output.push_back (colorList[i % colorList.size ()]);
        } else {
            output.push_back ("random");
        }
    }

    return output;
}


// Parse given name (3- or 6-digit hex or CSS3 names) as a color,
// return cairo-style 0.0-1.0 r,g,b tuples.
//
// This is unpleasantly verbose, but cairo etc. appear to be of no
// help.
//
// As a bonus, if the string cannot be parsed the usual way,
// synthesize a random one.  This way, metric names finding their way
// down here can get a persistent color.

struct rgb {
    double r;
    double g;
    double b;
    rgb (): r (0), g (0), b (0) {}
    rgb (short r, short g, short b): r (r/255.0), g (g/255.0), b (b/255.0) {}
};

void notcairo_parse_color (const string& name, double& r, double& g, double& b)
{
    // try to parse RRGGBB RGB #RRGGBB #RGB forms
    static const char hex[] = "0123456789ABCDEF";
    string name2;
    if ((name.size () == 4 || name.size () == 7) && (name[0] == '#')) {
        name2 = name.substr (1);    // clip off #
    } else {
        name2 = name;
    }

#define PARSE1(V,I) \
        const char *V = lower_bound (hex, hex+16, toupper(name2[I])); \
        if (*V != toupper(name2[I])) goto try_name;

    if (name2.size () == 6) { // try RRGGBB
        PARSE1 (r1,0);
        PARSE1 (r2,1);
        PARSE1 (g1,2);
        PARSE1 (g2,3);
        PARSE1 (b1,4);
        PARSE1 (b2,5);
        r = (r1 - hex) / 16.0 + (r2 - hex) / 16.0 / 15.0;
        g = (g1 - hex) / 16.0 + (g2 - hex) / 16.0 / 15.0;
        b = (b1 - hex) / 16.0 + (b2 - hex) / 16.0 / 15.0;
        return;
    } else if (name2.size () == 3) { // try RGB
        PARSE1 (r1,0);
        PARSE1 (g1,1);
        PARSE1 (b1,2);
        r = (r1 - hex) / 15.0;
        g = (g1 - hex) / 15.0;
        b = (b1 - hex) / 15.0;
        return;
    }
#undef PARSE1


try_name:
    // try to look up the name in the Official(tm) CSS3/SVG color map
    static map<string,rgb> colormap;
    if (colormap.size () == 0) {
        // http://www.w3.org/TR/SVG/types.html#ColorKeywords
        colormap["aliceblue"] = rgb (240, 248, 255);
        colormap["antiquewhite"] = rgb (250, 235, 215);
        colormap["aqua"] = rgb (0, 255, 255);
        colormap["aquamarine"] = rgb (127, 255, 212);
        colormap["azure"] = rgb (240, 255, 255);
        colormap["beige"] = rgb (245, 245, 220);
        colormap["bisque"] = rgb (255, 228, 196);
        colormap["black"] = rgb (0, 0, 0);
        colormap["blanchedalmond"] = rgb (255, 235, 205);
        colormap["blue"] = rgb (0, 0, 255);
        colormap["blueviolet"] = rgb (138, 43, 226);
        colormap["brown"] = rgb (165, 42, 42);
        colormap["burlywood"] = rgb (222, 184, 135);
        colormap["cadetblue"] = rgb (95, 158, 160);
        colormap["chartreuse"] = rgb (127, 255, 0);
        colormap["chocolate"] = rgb (210, 105, 30);
        colormap["coral"] = rgb (255, 127, 80);
        colormap["cornflowerblue"] = rgb (100, 149, 237);
        colormap["cornsilk"] = rgb (255, 248, 220);
        colormap["crimson"] = rgb (220, 20, 60);
        colormap["cyan"] = rgb (0, 255, 255);
        colormap["darkblue"] = rgb (0, 0, 139);
        colormap["darkcyan"] = rgb (0, 139, 139);
        colormap["darkgoldenrod"] = rgb (184, 134, 11);
        colormap["darkgray"] = rgb (169, 169, 169);
        colormap["darkgreen"] = rgb (0, 100, 0);
        colormap["darkgrey"] = rgb (169, 169, 169);
        colormap["darkkhaki"] = rgb (189, 183, 107);
        colormap["darkmagenta"] = rgb (139, 0, 139);
        colormap["darkolivegreen"] = rgb (85, 107, 47);
        colormap["darkorange"] = rgb (255, 140, 0);
        colormap["darkorchid"] = rgb (153, 50, 204);
        colormap["darkred"] = rgb (139, 0, 0);
        colormap["darksalmon"] = rgb (233, 150, 122);
        colormap["darkseagreen"] = rgb (143, 188, 143);
        colormap["darkslateblue"] = rgb (72, 61, 139);
        colormap["darkslategray"] = rgb (47, 79, 79);
        colormap["darkslategrey"] = rgb (47, 79, 79);
        colormap["darkturquoise"] = rgb (0, 206, 209);
        colormap["darkviolet"] = rgb (148, 0, 211);
        colormap["deeppink"] = rgb (255, 20, 147);
        colormap["deepskyblue"] = rgb (0, 191, 255);
        colormap["dimgray"] = rgb (105, 105, 105);
        colormap["dimgrey"] = rgb (105, 105, 105);
        colormap["dodgerblue"] = rgb (30, 144, 255);
        colormap["firebrick"] = rgb (178, 34, 34);
        colormap["floralwhite"] = rgb (255, 250, 240);
        colormap["forestgreen"] = rgb (34, 139, 34);
        colormap["fuchsia"] = rgb (255, 0, 255);
        colormap["gainsboro"] = rgb (220, 220, 220);
        colormap["ghostwhite"] = rgb (248, 248, 255);
        colormap["gold"] = rgb (255, 215, 0);
        colormap["goldenrod"] = rgb (218, 165, 32);
        colormap["gray"] = rgb (128, 128, 128);
        colormap["grey"] = rgb (128, 128, 128);
        colormap["green"] = rgb (0, 128, 0);
        colormap["greenyellow"] = rgb (173, 255, 47);
        colormap["honeydew"] = rgb (240, 255, 240);
        colormap["hotpink"] = rgb (255, 105, 180);
        colormap["indianred"] = rgb (205, 92, 92);
        colormap["indigo"] = rgb (75, 0, 130);
        colormap["ivory"] = rgb (255, 255, 240);
        colormap["khaki"] = rgb (240, 230, 140);
        colormap["lavender"] = rgb (230, 230, 250);
        colormap["lavenderblush"] = rgb (255, 240, 245);
        colormap["lawngreen"] = rgb (124, 252, 0);
        colormap["lemonchiffon"] = rgb (255, 250, 205);
        colormap["lightblue"] = rgb (173, 216, 230);
        colormap["lightcoral"] = rgb (240, 128, 128);
        colormap["lightcyan"] = rgb (224, 255, 255);
        colormap["lightgoldenrodyellow"] = rgb (250, 250, 210);
        colormap["lightgray"] = rgb (211, 211, 211);
        colormap["lightgreen"] = rgb (144, 238, 144);
        colormap["lightgrey"] = rgb (211, 211, 211);
        colormap["lightpink"] = rgb (255, 182, 193);
        colormap["lightsalmon"] = rgb (255, 160, 122);
        colormap["lightseagreen"] = rgb (32, 178, 170);
        colormap["lightskyblue"] = rgb (135, 206, 250);
        colormap["lightslategray"] = rgb (119, 136, 153);
        colormap["lightslategrey"] = rgb (119, 136, 153);
        colormap["lightsteelblue"] = rgb (176, 196, 222);
        colormap["lightyellow"] = rgb (255, 255, 224);
        colormap["lime"] = rgb (0, 255, 0);
        colormap["limegreen"] = rgb (50, 205, 50);
        colormap["linen"] = rgb (250, 240, 230);
        colormap["magenta"] = rgb (255, 0, 255);
        colormap["maroon"] = rgb (128, 0, 0);
        colormap["mediumaquamarine"] = rgb (102, 205, 170);
        colormap["mediumblue"] = rgb (0, 0, 205);
        colormap["mediumorchid"] = rgb (186, 85, 211);
        colormap["mediumpurple"] = rgb (147, 112, 219);
        colormap["mediumseagreen"] = rgb (60, 179, 113);
        colormap["mediumslateblue"] = rgb (123, 104, 238);
        colormap["mediumspringgreen"] = rgb (0, 250, 154);
        colormap["mediumturquoise"] = rgb (72, 209, 204);
        colormap["mediumvioletred"] = rgb (199, 21, 133);
        colormap["midnightblue"] = rgb (25, 25, 112);
        colormap["mintcream"] = rgb (245, 255, 250);
        colormap["mistyrose"] = rgb (255, 228, 225);
        colormap["moccasin"] = rgb (255, 228, 181);
        colormap["navajowhite"] = rgb (255, 222, 173);
        colormap["navy"] = rgb (0, 0, 128);
        colormap["oldlace"] = rgb (253, 245, 230);
        colormap["olive"] = rgb (128, 128, 0);
        colormap["olivedrab"] = rgb (107, 142, 35);
        colormap["orange"] = rgb (255, 165, 0);
        colormap["orangered"] = rgb (255, 69, 0);
        colormap["orchid"] = rgb (218, 112, 214);
        colormap["palegoldenrod"] = rgb (238, 232, 170);
        colormap["palegreen"] = rgb (152, 251, 152);
        colormap["paleturquoise"] = rgb (175, 238, 238);
        colormap["palevioletred"] = rgb (219, 112, 147);
        colormap["papayawhip"] = rgb (255, 239, 213);
        colormap["peachpuff"] = rgb (255, 218, 185);
        colormap["peru"] = rgb (205, 133, 63);
        colormap["pink"] = rgb (255, 192, 203);
        colormap["plum"] = rgb (221, 160, 221);
        colormap["powderblue"] = rgb (176, 224, 230);
        colormap["purple"] = rgb (128, 0, 128);
        colormap["red"] = rgb (255, 0, 0);
        colormap["rose"] = rgb (200,150,200); // a graphite-special
        colormap["rosybrown"] = rgb (188, 143, 143);
        colormap["royalblue"] = rgb (65, 105, 225);
        colormap["saddlebrown"] = rgb (139, 69, 19);
        colormap["salmon"] = rgb (250, 128, 114);
        colormap["sandybrown"] = rgb (244, 164, 96);
        colormap["seagreen"] = rgb (46, 139, 87);
        colormap["seashell"] = rgb (255, 245, 238);
        colormap["sienna"] = rgb (160, 82, 45);
        colormap["silver"] = rgb (192, 192, 192);
        colormap["skyblue"] = rgb (135, 206, 235);
        colormap["slateblue"] = rgb (106, 90, 205);
        colormap["slategray"] = rgb (112, 128, 144);
        colormap["slategrey"] = rgb (112, 128, 144);
        colormap["snow"] = rgb (255, 250, 250);
        colormap["springgreen"] = rgb (0, 255, 127);
        colormap["steelblue"] = rgb (70, 130, 180);
        colormap["tan"] = rgb (210, 180, 140);
        colormap["teal"] = rgb (0, 128, 128);
        colormap["thistle"] = rgb (216, 191, 216);
        colormap["tomato"] = rgb (255, 99, 71);
        colormap["turquoise"] = rgb (64, 224, 208);
        colormap["violet"] = rgb (238, 130, 238);
        colormap["wheat"] = rgb (245, 222, 179);
        colormap["white"] = rgb (255, 255, 255);
        colormap["whitesmoke"] = rgb (245, 245, 245);
        colormap["yellow"] = rgb (255, 255, 0);
        colormap["yellowgreen"] = rgb (154, 205, 50);
    }

    map<string,rgb>::iterator it = colormap.find (name);
    if (it != colormap.end ()) {
        r = it->second.r;
        g = it->second.g;
        b = it->second.b;
        return;
    }

    // no idea ... generate a random color
    r = (random () % 256) / 255.0;
    g = (random () % 256) / 255.0;
    b = (random () % 256) / 255.0;
}


// Heuristically compute some reasonably rounded minimum/maximum
// values and major tick lines for the vertical scale.
//
// Algorithm based on Label.c / Paul Heckbert / "Graphics Gems",
// Academic Press, 1990
//
// EULA: The Graphics Gems code is copyright-protected. In other
// words, you cannot claim the text of the code as your own and resell
// it. Using the code is permitted in any program, product, or
// library, non-commercial or commercial. Giving credit is not
// required, though is a nice gesture.

float nicenum (float x, bool round_p)
{
    int expv;/* exponent of x */
    double f;/* fractional part of x */
    double nf;/* nice, rounded fraction */

    expv = (int) floor (log10f (x));
    f = x/powf (10., expv); /* between 1 and 10 */
    if (round_p)
        if (f<1.5) {
            nf = 1.;
        } else if (f<3.) {
            nf = 2.;
        } else if (f<7.) {
            nf = 5.;
        } else {
            nf = 10.;
        }
    else if (f<=1.) {
        nf = 1.;
    } else if (f<=2.) {
        nf = 2.;
    } else if (f<=5.) {
        nf = 5.;
    } else {
        nf = 10.;
    }
    return nf*powf (10., expv);
}

vector<float> round_linear (float& ymin, float& ymax, unsigned nticks)
{
    vector<float> ticks;

    // make some space between min & max
    float epsilon = 0.5;
    if ((ymax - ymin) < epsilon) {
        ymin -= epsilon;
        ymax += epsilon;
    }

    if (nticks <= 1) {
        nticks = 3;
    }

    float range = nicenum (ymax-ymin, false);
    float d = nicenum (range/ (nticks-1), true);
    ymin = floorf (ymin/d)*d;
    ymax = ceilf (ymax/d)*d;

    // NB: just adding d to ymin in a loop accumulates errors
    // faster than this formulation; esp. for the 0 value
    float x = ymin;
    for (unsigned i=0; x <= ymax; x = ymin + (++i) * d) {
        ticks.push_back (x);
    }

    return ticks;
}



time_t nicetime (time_t x, bool round_p, char const **fmt)
{
    static const time_t powers[] = {1, 30, // seconds
                                    60, 60*5, 60*10, 60*30, 60*60, // minutes
                                    60*60*2, 60*60*4, 60*60*6, 60*60*12, 60*60*24, // hours
                                    60*60*24*7, 60*60*24*7*4,  60*60*24*7*52
                                   }; // weeks
    unsigned npowers = sizeof (powers)/sizeof (powers[0]);
    time_t ex;
    for (int i=npowers-1; i>=0; i--) {
        ex = powers[i];
        if (ex <= x) {
            break;
        }
    }

    time_t result;
    if (round_p) {
        result = ((x + ex - 1) / ex) * ex;
    } else {
        result = (x / ex) * ex;
    }

    // clog << "round time=" << x << " result=" << result << endl;
    if (fmt) { // compute an appropriate date-rendering strftime format
        if (result < 60) { // minute
            *fmt = "%H:%M:%S";
        } else if (result < 60*60) { // hour
            *fmt = "%H:%M";
        } else if (result < 24*60*60) { // day
            *fmt = "%a %H:%M";
        } else if (result < 7*24*60*60) { // week
            *fmt = "%m-%d %H:%M";
        } else if (result < 365*24*60*60) { // year
            *fmt = "%m-%d";
        } else { // larger than a year
            *fmt = "%Y-%m-%d";
        }
    }

    return result;
}



vector<time_t> round_time (time_t xmin, time_t xmax, unsigned nticks, char const** fmt)
{
    (void) fmt;
    vector<time_t> ticks;

    // make some space between min & max
    time_t epsilon = 1;
    if ((xmax - xmin) < epsilon) {
        xmin -= epsilon;
        xmax += epsilon;
    }

    if (nticks <= 1) {
        nticks = 2;
    }

    time_t range = nicetime (xmax-xmin, false, NULL);
    time_t d = nicetime (range/ (nticks+1), true, fmt);
    xmin = ((xmin + d - 1)/ d) * d;
    xmax = ((xmax + d - 1)/ d) * d;

    for (time_t x = xmin; x < xmax; x += d) {
        ticks.push_back (x);
    }

    return ticks;
}



// A sorting-comparator object for computing rankings.
template <typename Type>
struct ranking_comparator {
    ranking_comparator (const vector<Type>& d): data (d) {}
    int operator () (unsigned i, unsigned j) {
        assert (data.size () > i && data.size () > j);
        return data[i] > data[j];   // we'd like reverse order
    }
private:
    const vector<Type>& data;
};



// Render archive data to an image.  It doesn't need to be as pretty
// as the version built into the graphite server-side code, just
// serviceable.

int
pmgraphite_respond_render_gfx (struct MHD_Connection *connection,
                               const http_params & params, const vector <string> &url, const string& url0)
{
    int rc;
    struct MHD_Response *resp;
    string imgbuf;
    cairo_status_t cst;
    cairo_surface_t *sfc;
    cairo_t *cr;
    vector<timeseries> all_results;
    vector<pmDesc> all_result_descs;
    string colorList;
    vector<string> colors;
    string bgcolor;
    double graphxlow, graphxhigh, graphylow, graphyhigh;
    vector<float> yticks;
    vector<time_t> xticks;
    char const *strf_format = "%s"; // by default, epoch seconds
    vector<unsigned> total_visibility_score;
    vector<unsigned> visibility_rank;

    string format = params["format"];
    if (format == "") {
        format = "png";
    }

    // SVG? etc?
    if (format != "png") {
        return mhd_notify_error (connection, -EINVAL);
    }

    vector<metric_string> targets;
    time_t t_start, t_end, t_step;
    int t_relative_p;
    rc = pmgraphite_gather_data (connection, params, url, targets, t_start, t_end, t_step, t_relative_p);
    if (rc) {
        return mhd_notify_error (connection, rc);
    }

    // If this had relative timestamps, return a redirect HTTP
    // request, so that the IMG URL is absolute, thus the PNG results
    // are cachable.
    //
    if (t_relative_p) {
        stringstream new_url;

        // Send back the EPOCH absolute-times
        new_url << url0 << "?from=" << t_start << "&until=" << t_end;

        // Propagate other incoming query-string parameters.
        if (! params.empty ()) {
            for (http_params::const_iterator it = params.begin ();
                 it != params.end ();
                 it++) {

                // skip these incoming parameters; we're replacing them
                if (it->first == "from" || it->first == "until")
                    continue;

                new_url << "&" << it->first << "=";
                if (it->second != "") {
                    new_url << urlencode (it->second);
                }
            }
        }

        static char blank[] = "";
        resp = MHD_create_response_from_buffer (strlen (blank), blank, MHD_RESPMEM_PERSISTENT);
        if (resp) {
            rc = MHD_add_response_header (resp, "Location", new_url.str ().c_str ());
            if (rc != MHD_YES) {
                connstamp (cerr, connection) << "MHD_add_response_header Location: failed" << endl;
            }

            /* Adding ACAO header */
            (void) MHD_add_response_header (resp, "Access-Control-Allow-Origin", "*");

            rc = MHD_queue_response (connection, MHD_HTTP_FOUND /* 302 */ , resp);
            if (rc != MHD_YES) {
                connstamp (cerr, connection) << "MHD_queue_response failed" << endl;
            }
            MHD_destroy_response (resp);
            return MHD_YES;
        }
        return MHD_NO;		// general 500 error

        // NB: we could fall through to non-redirected handling as in previous versions
        // of pmwebd, but if MHD_create_response_from_buffer failed, chances are the
        // following will fail too.
    }

    // Gather up all the data.  We need several passes over it, so gather it into a vector<vector<> >.
    (void) pmgraphite_fetch_all_series (connection, targets, all_results, all_result_descs, t_start, t_end, t_step);

    if (exit_p) {
        return MHD_NO;
    }

    if (params["drawNullAsZero"] == "true") {
        for (unsigned i=0; i<all_results.size (); i++)
            timeseries_nullconvert (all_results[i], 0);
    }

    int width = atoi (params["width"].c_str ());
    if (width <= 0) {
        width = 640;
    }
    int height = atoi (params["height"].c_str ());
    if (height <= 0) {
        height = 480;
    }

    sfc = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
    if (sfc == NULL) {
        rc = -ENOMEM;
        goto out1;
    }

    cr = cairo_create (sfc);
    if (cr == NULL) {
        rc = -ENOMEM;
        goto out2;
    }

    double r, g, b;
    bgcolor = params["bgcolor"];
    if (bgcolor == "")
        // as per graphite render/glyph.py defaultGraphOptions
    {
        bgcolor="white";
    }
    notcairo_parse_color (bgcolor, r, g, b);
    cairo_save (cr);
    cairo_set_source_rgb (cr, r, g, b);
    cairo_rectangle (cr, 0.0, 0.0, width, height);
    cairo_fill (cr);
    cairo_restore (cr);

    // Compute vertical bounds.
    float ymin;
    if (params["yMin"] != "") {
        ymin = atof (params["yMin"].c_str ());
    } else {
        ymin = nanf ("");
        for (unsigned i=0; i<all_results.size (); i++)
            for (unsigned j=0; j<all_results[i].size (); j++) {
                if (! isfinite (all_results[i][j])) {
                    continue;
                }
                if (! isfinite (ymin)) {
                    ymin = all_results[i][j];
                } else {
                    ymin = min (ymin, all_results[i][j]);
                }
            }
    }
    float ymax;
    if (params["yMax"] != "") {
        ymax = atof (params["yMax"].c_str ());
    } else {
        ymax = nanf ("");
        for (unsigned i=0; i<all_results.size (); i++)
            for (unsigned j=0; j<all_results[i].size (); j++) {
                if (! isfinite (all_results[i][j])) {
                    continue;
                }
                if (! isfinite (ymax)) {
                    ymax = all_results[i][j];
                } else {
                    ymax = max (ymax, all_results[i][j]);
                }
            }
    }

    // Any data to show?
    if (! isfinite (ymin) || ! isfinite (ymax) || all_results.empty ()) {
        cairo_text_extents_t ext;
        string message = "no data in range";
        cairo_save (cr);
        cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size (cr, min(height/4.0, width/15.0));
        cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
        cairo_text_extents (cr, message.c_str (), &ext);
        cairo_move_to (cr,
                       width/2.0 - (ext.width/2 + ext.x_bearing),
                       height/2.0 - (ext.height/2 + ext.y_bearing));
        cairo_show_text (cr, message.c_str ());
        cairo_restore (cr);
        goto render_done;
    }


    // What makes us tick?
    yticks = round_linear (ymin, ymax,
                           (unsigned) (min (height/(8*1.5), 0.3 * sqrtf (height))));
    xticks = round_time (t_start, t_end,
                         (unsigned) (min (width/(8*10.0), 0.3 * sqrtf (width))),
                         & strf_format);


    if (verbosity > 2)
        connstamp (clog, connection) << "rendering " << all_results.size () << " metrics"
                                     << ", ymin=" << ymin << ", ymax=" << ymax << endl;

    // Because we're going for basic acceptable rendering only, for
    // purposes of graphite-builder previews, we hard-code a simple
    // layout scheme:
    if (params["graphOnly"] != "true") {
        graphxlow = width * 0.1;
        graphxhigh = width * 0.9;
        graphylow = height * 0.05;
        graphyhigh = height * 0.95;
    } else { // graphOnly mode - go right to the edges
        graphxlow = 0;
        graphxhigh = width;
        graphylow = 0;
        graphyhigh = height;
    }
    // ... though these numbers might be adjusted a bit by legend etc. rendering

    // As a mnemonic, double typed variables are used to track
    // coordinates in graphics space, and float (with nan) for pcp
    // metric space.

    // Fetch curve color list
    colorList = params["colorList"];
    if (colorList == "") {
        // as per graphite render/glyph.py defaultGraphOptions
        colorList = "blue,green,red,purple,brown,yellow,aqua,grey,magenta,pink,gold,mistyrose";
    }
    colors = generate_colorlist (split (colorList,','), targets.size ());
    assert (colors.size () == targets.size ());

    // Draw the title
    if (params["title"] != "") {
        const string& title = params["title"];
        double r, g, b;
        double spacing = 15.0;
        double baseline = spacing;
        cairo_text_extents_t ext;

        // draw text
        cairo_save (cr);
        string fgcolor = params["fgcolor"];
        if (fgcolor == "") {
            fgcolor = "black";
        }
        notcairo_parse_color (fgcolor, r, g, b);
        cairo_set_source_rgb (cr, r, g, b);
        cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size (cr, spacing);

        cairo_text_extents (cr, title.c_str (), &ext);
        cairo_move_to (cr,
                       width/2.0 - (ext.width/2 + ext.x_bearing),
                       baseline + ext.height + ext.y_bearing);
        cairo_show_text (cr, title.c_str ());
        cairo_restore (cr);

        // allocate space for the graph
        baseline += spacing*1.2;

        if (graphylow < baseline) {
            graphylow = baseline;
        }
    }

    // We'd like to draw the legends of those curves that are
    // largely in view.  In particular, for curves that come from
    // archives outside the timeline, or for those who are
    // effectively invisible due to a lot of proximity to other
    // curves points, we'd like to deemphasize them in the legend.
    //
    // It's almost like wanting to put the curves in decreasing
    // maximum-Frechet-Distance-to-others order, but that takes too
    // much math.
    //
    for (unsigned i=0; i<all_results.size (); i++) {
        total_visibility_score.push_back (0);
        const timeseries& f = all_results[i];
        for (unsigned j=0; j<f.size (); j++) {
            if (! isfinite (f[j])) {
                continue;
            }
            total_visibility_score[i] ++;

            // XXX: give extra points if this data point is far
            // those from other non-[i] curves.  But that's bound
            // to be computationally expensive to do it Right(tm).
            //
            // So we give a simple estimate of vertical distance only.

            for (unsigned k=0; k<all_results.size (); k++) {
                const timeseries& f2 = all_results[k];
                if (i == k) // same time series?
                    continue;

                if (f2.size() <= j) // small vector due to fetch errors?
                    continue;

                if (! isfinite (f2[j])) // missing data item?
                    continue;

                assert (f2.when(j) == f.when(j));
                float delta = f2[j] - f[j];
                float reldelta = fabs (delta / (ymax - ymin)); // compare delta to height of graph
                assert (reldelta >= 0.0 && reldelta <= 1.0);
                unsigned points = (unsigned) (reldelta * 10);
                total_visibility_score[i] += points;
            }

        }
    }

    for (unsigned i=0; i<total_visibility_score.size (); i++) {
        visibility_rank.push_back (i);
    }
    sort (visibility_rank.begin (), visibility_rank.end (),
          ranking_comparator<unsigned> (total_visibility_score));


    // Draw the legend
    if (params["hideLegend"] != "true" && params["graphOnly"] != "true" &&
            (params["hideLegend"] == "false" || targets.size () <= 10)) { // maximum number of legend entries
        double spacing = 10.0;
        double baseline = height - 8.0;
        double leftedge = 10.0;

        for (unsigned i=0; i<visibility_rank.size (); i++) {
            // compute legend string
            const metric_string& metric_name = targets[visibility_rank[i]];
            const char* metric_units = pmUnitsStr(&all_result_descs[visibility_rank[i]].units);
            string name;

            if (metric_units && metric_units[0]) // likely
                name = string(metric_name.unsplit()) + " (" + string(metric_units) + ")";
            else
                name = metric_name.unsplit();

            if (all_result_descs[visibility_rank[i]].sem == PM_SEM_COUNTER)
                name += " rate";

            double r, g, b;

            // don't even bother put this on the legend if it has zero information
            if (total_visibility_score[visibility_rank[i]] == 0) {
                continue;
            }

            // draw square swatch
            cairo_save (cr);
            notcairo_parse_color (colors[i], r, g, b);
            cairo_set_source_rgb (cr, r, g, b);
            cairo_rectangle (cr, leftedge+1.0, baseline+1.0, spacing-1.0, 0.0-spacing-1.0);
            cairo_fill (cr);
            cairo_restore (cr);

            // draw text
            cairo_save (cr);
            string fgcolor = params["fgcolor"];
            if (fgcolor == "") {
                fgcolor = "black";
            }
            notcairo_parse_color (fgcolor, r, g, b);
            cairo_set_source_rgb (cr, r, g, b);
            cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size (cr, spacing);
            cairo_move_to (cr, leftedge + spacing*1.5, baseline);
            cairo_show_text (cr, name.c_str ());
            cairo_restore (cr);

            // allocate space for next row up
            baseline -= spacing * 1.2;
            if (graphyhigh > baseline) {
                graphyhigh = baseline - spacing;
            }
            if (graphyhigh < height*0.6) { // forget it, don't go beyond 40%
                break;
            }
        }
    }

    if (params["graphOnly"] != "true") {
        // Shrink the graph to make room for axis labels; match apprx. layout to
        // grafana's javascript-side rendering engine
        graphxlow = 5 * 8.0; // some digits of axis-label width
        graphxhigh = width * 0.96;
        graphyhigh -= 10.;

        double r, g, b;
        double line_width = 1.5;

        // Draw the grid
        cairo_save (cr);
        string majorGridLineColor = params["majorGridLineColor"];
        if (majorGridLineColor == "") {
            majorGridLineColor = "pink";
        }
        notcairo_parse_color (majorGridLineColor, r, g, b);
        cairo_set_source_rgb (cr, r, g, b);
        cairo_set_line_width (cr, line_width/2);

        // Y axis grid & labels
        if (params["hideYAxis"] != "true" && params["graphOnly"] != "true")
            for (unsigned i=0; i<yticks.size (); i++) {
                float thisy = yticks[i];
                float ydelta = (ymax - ymin);
                double rely = (double)ymax/ydelta - (double)thisy/ydelta;
                double y = graphylow + (graphyhigh-graphylow)*rely;

                cairo_move_to (cr, graphxlow, y);
                if (params["hideGrid"] != "true") {
                    cairo_line_to (cr, graphxhigh, y);
                } else {
                    cairo_line_to (cr, graphxlow+4, y);
                }
                cairo_stroke (cr);

                stringstream label;
                label << setprecision(5) << yticks[i];
                string lstr = label.str ();
                cairo_text_extents_t ext;
                cairo_save (cr);
                cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size (cr, 8.0);
                string fgcolor = params["fgcolor"];
                if (fgcolor == "") {
                    fgcolor = "black";
                }
                notcairo_parse_color (fgcolor, r, g, b);
                cairo_set_source_rgb (cr, r, g, b);
                cairo_text_extents (cr, lstr.c_str (), &ext);
                cairo_move_to (cr,
                               graphxlow - (ext.width + ext.x_bearing) - line_width*3,
                               y - (ext.height/2 + ext.y_bearing));
                cairo_show_text (cr, lstr.c_str ());
                cairo_restore (cr);
            }

        // X axis grid & labels
        if (params["hideAxes"] != "true" && params["graphOnly"] != "true")
            for (unsigned i=0; i<xticks.size (); i++) {
                float xdelta = (t_end - t_start);
                double relx = (double) (xticks[i]-t_start)/xdelta;
                double x = graphxlow + (graphxhigh-graphxlow)*relx;

                cairo_move_to (cr, x, graphyhigh);
                if (params["hideGrid"] != "true")
                    cairo_line_to (cr, x, graphylow);
                else
                    cairo_line_to (cr, x, graphyhigh-4);
                cairo_stroke (cr);

                // We use gmtime / strftime to make a compact rendering of
                // the (UTC) time_t.
                char timestr[100];
                struct tm *t = gmtime (& xticks[i]);
                strftime (timestr, sizeof (timestr), strf_format, t);

                cairo_text_extents_t ext;
                cairo_save (cr);
                cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size (cr, 8.0);
                string fgcolor = params["fgcolor"];
                if (fgcolor == "") {
                    fgcolor = "black";
                }
                notcairo_parse_color (fgcolor, r, g, b);
                cairo_set_source_rgb (cr, r, g, b);
                cairo_text_extents (cr, timestr, &ext);
                cairo_move_to (cr,
                               x - (ext.width/2 + ext.x_bearing),
                               graphyhigh + (ext.height + ext.y_bearing) + 10);
                cairo_show_text (cr, timestr);
                cairo_restore (cr);
            }
        cairo_restore (cr); // line width

        // Draw the frame (on top of the funky pink grid)
        cairo_save (cr);
        string fgcolor = params["fgcolor"];
        if (fgcolor == "") {
            fgcolor = "black";
        }
        notcairo_parse_color (fgcolor, r, g, b);
        cairo_set_source_rgb (cr, r, g, b);
        cairo_set_line_width (cr, line_width*1.5);
        cairo_rectangle (cr, graphxlow, graphylow, (graphxhigh-graphxlow), (graphyhigh-graphylow));
        cairo_stroke (cr);
        cairo_restore (cr);
    }

    // Draw the curves, in *increasing* visibility order, letting
    // higher-score curves draw on top of the lower ones
    for (int i=visibility_rank.size ()-1; i>=0; i--) {
        // don't even waste time trying to draw this curve
        if (total_visibility_score[visibility_rank[i]] == 0) {
            continue;
        }

        const timeseries& f = all_results[visibility_rank[i]];

        double r,g,b;
        cairo_save (cr);
        notcairo_parse_color (colors[i], r, g, b); // NB: match legend!
        cairo_set_source_rgb (cr, r, g, b);

        string lineWidth = params["lineWidth"];
        if (lineWidth == "") {
            lineWidth = "1.2";
        }
        double line_width = atof (lineWidth.c_str ());
        cairo_set_line_width (cr, line_width);
        cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

#if 0
        // transform cairo rendering coordinate system, so we can feed
        // it raw data timestamp/value pairs and get them drawn onto
        // the designated graph[xy]{low,high} region.
        cairo_translate (cr, (double)graphxlow, (double)graphylow);
        cairo_scale (cr, (double)graphxhigh-graphxlow, (double)graphyhigh-graphylow);
        cairo_scale (cr, 1./ ((double)t_end- (double)t_start), 1./ ((double)ymax- (double)ymin));
        cairo_translate (cr, - (double)t_start, - (double)ymin);

        // XXX: unfortunately, the order of operations or something else is awry with the above.
#endif

        float lastx = nanf ("");
        float lasty = nanf ("");
        for (unsigned j=0; j<f.size (); j++) {
            float thisy = f[j];

            // clog << "(" << lastx << "," << lasty << ")";

            if (! isfinite (thisy)) {
                // This data slot is missing, so put a circle at the previous end, if
                // possible, to indicate the discontinuity
                if (isfinite (lastx) && isfinite (lasty)) {
                    cairo_move_to (cr, lastx, lasty);
                    cairo_arc (cr, lastx, lasty, line_width*0.5, 0., 2*M_PI);
                    cairo_stroke (cr);
                }

                // disconnect at null points unless requested
                if (params["lineMode"] != "connected")
                    lasty = nanf ("");

                continue;
            }

            float xdelta = (t_end - t_start);
            float ydelta = (ymax - ymin);
            double relx = (double) (f.when(j) - t_start)/xdelta;
            double rely = (double)ymax/ydelta - (double)thisy/ydelta;
            float x = graphxlow + (graphxhigh-graphxlow)*relx; // scaled into graphics grid area
            float y = graphylow + (graphyhigh-graphylow)*rely;

            // clog << "-(" << x << "," << y << ") ";

            cairo_move_to (cr, x, y);
            if (isfinite (lastx) && isfinite (lasty)) {
                // draw it as a line
                cairo_line_to (cr, lastx, lasty);
            } else {
                // draw it as a circle
                cairo_arc (cr, x, y, line_width*0.5, 0., 2*M_PI);
            }
            cairo_stroke (cr);

            lastx = x;
            lasty = y;
        }

        // clog << endl;
        cairo_restore (cr);
    }


    // Need to get the data out of cairo and into microhttpd.  We use
    // a c++ string as a temporary, which can carry binary data.
render_done:
    cst = cairo_surface_write_to_png_stream (sfc, & notcairo_write_to_string, (void *) & imgbuf);
    if (cst != CAIRO_STATUS_SUCCESS || cairo_status (cr) != CAIRO_STATUS_SUCCESS) {
        rc = -EIO;
        goto out3;
    }

    /* NB: no point trying to compress the PNG via NOTMHD_compressible_response. */
    resp = MHD_create_response_from_buffer (imgbuf.size (), (void*) imgbuf.c_str (),
                                            MHD_RESPMEM_MUST_COPY);
    if (resp == NULL) {
        connstamp (cerr, connection) << "MHD_create_response_from_buffer failed" << endl;
        rc = -ENOMEM;
        goto out3;
    }

    /* https://developer.mozilla.org/en-US/docs/HTTP/Access_control_CORS */
    rc = MHD_add_response_header (resp, "Access-Control-Allow-Origin", "*");
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_add_response_header ACAO failed" << endl;
        rc = -ENOMEM;
        goto out4;
    }

    rc = MHD_add_response_header (resp, "Content-Type", "image/png");
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_add_response_header CT failed" << endl;
        rc = -ENOMEM;
        goto out4;
    }

    // NB: we used to emit some caching-suppression headers,
    // but with the 302 absolute-time redirects, it should not be
    // necessary.
#if 0
    (void) MHD_add_response_header (resp, "Cache-Control", "no-cache");
    (void) MHD_add_response_header (resp, "Cache-Directive", "no-cache");
    (void) MHD_add_response_header (resp, "Pragma", "no-cache");
    (void) MHD_add_response_header (resp, "Expires", "0");
#endif

    rc = MHD_queue_response (connection, MHD_HTTP_OK, resp);
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_queue_response failed" << endl;
        rc = -ENOMEM;
        goto out4;
    }

    rc = 0;

out4:
    MHD_destroy_response (resp);
out3:
    cairo_destroy (cr);
out2:
    cairo_surface_destroy (sfc);
out1:
    if (rc) {
        return mhd_notify_error (connection, rc);
    } else {
        return MHD_YES;
    }
}


#endif /* HAVE_CAIRO */

/* ------------------------------------------------------------------------ */


// Render raw archive data in JSON form.
int
pmgraphite_respond_render_json (struct MHD_Connection *connection,
                                const http_params & params, const vector <string> &url,
                                bool rawdata_flavour_p)
{
    int rc;
    struct MHD_Response *resp;

    vector<metric_string> targets;
    time_t t_start, t_end, t_step;
    int t_relative_p;
    rc = pmgraphite_gather_data (connection, params, url, targets, t_start, t_end, t_step, t_relative_p);
    if (rc) {
        return mhd_notify_error (connection, rc);
    }

    vector<timeseries> all_results; // indexed as targets[]
    vector<pmDesc> all_result_descs; // indexed as targets[]
    pmgraphite_fetch_all_series (connection, targets, all_results, all_result_descs, t_start, t_end, t_step);

    stringstream output;
    output << "[";
    for (unsigned k = 0; k < targets.size (); k++) {
        const metric_string& target = targets[k];
        const timeseries& results = all_results[k];

        if (k > 0) {
            output << ",";
        }

        // NB: as an optimization, we could filter out targets with all-null results[].
        
        if (rawdata_flavour_p) { // a vector of values with implicit timestamp - can't elide null runs
            output << "{";
            json_key_value (output, "start", t_start, ",");
            json_key_value (output, "step", t_step, ",");
            json_key_value (output, "end", t_end, ",");
            json_key_value (output, "name", string(target.unsplit()), ",");
            output << " \"data\":[";
            for (unsigned i = 0; i < results.size (); i++) {
                if (i > 0) {
                    output << ",";
                }
                if (! isfinite (results[i])) {
                    output << "null";
                } else {
                    // Setting the output.precision() not so necessary here
                    // as in the pmwebapi case, since the data is already narrowed
                    // to a float, and default precision of 6 works fine.
                    output << results[i];
                }
            }
            output << "]}";

        } else { // a timestamped vector of values
            output << "{";
            json_key_value (output, "target", string(target.unsplit()), ",");
            output << " \"datapoints\":[";

            // We can elide runs of nulls, but carefully: we can
            // remove the middle but not first & last nulls in a run.
            // In some experiments, other tunings resulted in grafana
            // 1.9.1 complaints about "Datapoints outside time range".

            unsigned printed = 0;
            for (unsigned i = 0; i < results.size (); i++) {
                if (! isfinite (results[i]) && // NaN
                    // not sandwiched between NaNs
                    ! (((i>0) && !isfinite (results[i-1])) &&
                       ((i<results.size()-1) && !isfinite (results[i+1])))) {
                    if (printed++)
                        output << ",";
                    output << "[null," << results.when(i) << "]";
                } else if (isfinite (results[i])) {
                    if (printed++)
                        output << ",";
                    output << "[" << results[i] << "," << results.when(i) << "]";
                }
            }
            output << "]}";
        }

    }
    output << "]";

    // wrap it up in mhd response ribbons
    resp = NOTMHD_compressible_response (connection, output.str());
    if (resp == NULL) {
        connstamp (cerr, connection) << "MHD_create_response_from_buffer failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    /* https://developer.mozilla.org/en-US/docs/HTTP/Access_control_CORS */
    rc = MHD_add_response_header (resp, "Access-Control-Allow-Origin", "*");
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_add_response_header ACAO failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    rc = MHD_add_response_header (resp, "Content-Type", "application/json");
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_add_response_header CT failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }
    rc = MHD_queue_response (connection, MHD_HTTP_OK, resp);
    if (rc != MHD_YES) {
        connstamp (cerr, connection) << "MHD_queue_response failed" << endl;
        rc = -ENOMEM;
        goto out1;
    }

    MHD_destroy_response (resp);
    return MHD_YES;

out1:
    return mhd_notify_error (connection, rc);
}


/* ------------------------------------------------------------------------ */


int
pmgraphite_respond (struct MHD_Connection *connection, const http_params & params,
                    const vector <string> &url, const string& url0)
{
    string url1 = (url.size () >= 2) ? url[1] : "";
    assert (url1 == "graphite");
    string url2 = (url.size () >= 3) ? url[2] : "";
    string url3 = (url.size () >= 4) ? url[3] : "";

    if (url2 == "rawdata") {
        // graphlot style
        return pmgraphite_respond_render_json (connection, params, url, true);
    } else if (url2 == "render" && params["format"] == "json") {
        // grafana style
        return pmgraphite_respond_render_json (connection, params, url, false);
    } else if (url2 == "metrics" && url3 == "find") {
        // grafana, graphite tree & auto-completer
        return pmgraphite_respond_metrics_find (connection, params, url);
    } else if (url2 == "graphlot" && url3 == "findmetric") {
        // graphlot
        return pmgraphite_respond_metrics_grep (connection, params, url, true);
    } else if (url2 == "browser" && url3 == "search") {
        // graphite search
        return pmgraphite_respond_metrics_grep (connection, params, url, false);
#ifdef HAVE_CAIRO
    } else if (url2 == "render") {
        return pmgraphite_respond_render_gfx (connection, params, url, url0);
#else
        (void) url0;
        // XXX: it would be nice to inform the user why we're failing
#endif
    }

    return mhd_notify_error (connection, -EINVAL);
}
