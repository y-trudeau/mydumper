/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    Domas Mituzas, Facebook ( domas at fb dot com )
                    Mark Leith, Oracle Corporation (mark dot leith at oracle dot com)
                    Andrew Hutchings, SkySQL (andrew at skysql dot com)
                    Max Bubenick, Percona RDBA (max dot bubenick at percona dot com)
                    David Ducos, Percona (david dot ducos at percona dot com)
*/

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <mysql.h>

#if defined MARIADB_CLIENT_VERSION_STR && !defined MYSQL_SERVER_VERSION
#define MYSQL_SERVER_VERSION MARIADB_CLIENT_VERSION_STR
#endif

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#ifdef ZWRAP_USE_ZSTD
#include "../zstd/zstd_zlibwrapper.h"
#else
#include <zlib.h>
#endif
#include <pcre.h>
#include <signal.h>
#include <glib/gstdio.h>
#include <glib/gerror.h>
#include <gio/gio.h>
#include "config.h"
#include "server_detect.h"
#include "connection.h"
//#include "common_options.h"
#include "common.h"
#include <glib-unix.h>
#include <math.h>
#include "logging.h"
#include "set_verbose.h"
#include "locale.h"
#include <sys/statvfs.h>

#include "tables_skiplist.h"
#include "regex.h"
#include "common.h"
#include "mydumper_start_dump.h"
#include "mydumper_jobs.h"
#include "mydumper_common.h"
#include "mydumper_stream.h"
#include "mydumper_database.h"
#include "mydumper_working_thread.h"
#include "mydumper_pmm_thread.h"
#include "mydumper_exec_command.h"
#include "mydumper_masquerade.h"
/* Some earlier versions of MySQL do not yet define MYSQL_TYPE_JSON */
#ifndef MYSQL_TYPE_JSON
#define MYSQL_TYPE_JSON 245
#endif

/* Program options */
extern GKeyFile * key_file;
extern gint database_counter;
extern GAsyncQueue *stream_queue;
extern gchar *output_directory;
extern gchar *output_directory_param;
extern gchar *dump_directory;
extern guint snapshot_count;
extern gboolean daemon_mode;
extern gchar *disk_limits;
extern gboolean load_data;
extern gboolean stream;
extern int detected_server;
extern gboolean no_delete;
extern char *defaults_file;
extern FILE * (*m_open)(const char *filename, const char *);
extern int (*m_close)(void *file);
extern int (*m_write)(FILE * file, const char * buff, int len);
extern gchar *db;
extern GString *set_session;
extern guint num_threads;
extern char **tables;
extern gchar *tables_skiplist_file;

gchar *tidb_snapshot = NULL;
GList *no_updated_tables = NULL;
int longquery = 60;
int longquery_retries = 0;
int longquery_retry_interval = 60;
int need_dummy_read = 0;
int need_dummy_toku_read = 0;
int compress_output = 0;
int killqueries = 0;
int lock_all_tables = 0;
gboolean no_schemas = FALSE;
gboolean no_locks = FALSE;
gboolean less_locking = FALSE;
gboolean no_backup_locks = FALSE;
gboolean no_ddl_locks = FALSE;
gboolean dump_tablespaces = FALSE;
//GList *innodb_tables = NULL;
GList *non_innodb_table = NULL;
GList *table_schemas = NULL;
GList *trigger_schemas = NULL;
GList *view_schemas = NULL;
GList *schema_post = NULL;
gint non_innodb_table_counter = 0;
gint non_innodb_done = 0;
guint updated_since = 0;
guint trx_consistency_only = 0;
gchar *set_names_str=NULL;
gchar *pmm_resolution = NULL;
gchar *pmm_path = NULL;
gboolean pmm = FALSE;
GHashTable *all_anonymized_function=NULL;
GHashTable *all_where_per_table=NULL;
guint pause_at=0;
guint resume_at=0;
gchar **db_items=NULL;

GMutex *ready_database_dump_mutex = NULL;

// For daemon mode
extern guint dump_number;
extern gboolean shutdown_triggered;
extern GAsyncQueue *start_scheduled_dump;

extern guint errors;

gchar *exec_command=NULL;

static GOptionEntry start_dump_entries[] = {
    {"compress", 'c', 0, G_OPTION_ARG_NONE, &compress_output,
     "Compress output files", NULL},
    {"exec", 0, 0, G_OPTION_ARG_STRING, &exec_command,
      "Command to execute using the file as parameter", NULL},
    {"long-query-retries", 0, 0, G_OPTION_ARG_INT, &longquery_retries,
     "Retry checking for long queries, default 0 (do not retry)", NULL},
    {"long-query-retry-interval", 0, 0, G_OPTION_ARG_INT, &longquery_retry_interval,
     "Time to wait before retrying the long query check in seconds, default 60", NULL},
    {"long-query-guard", 'l', 0, G_OPTION_ARG_INT, &longquery,
     "Set long query timer in seconds, default 60", NULL},    
    {"tidb-snapshot", 'z', 0, G_OPTION_ARG_STRING, &tidb_snapshot,
     "Snapshot to use for TiDB", NULL},
    {"updated-since", 'U', 0, G_OPTION_ARG_INT, &updated_since,
     "Use Update_time to dump only tables updated in the last U days", NULL},
    {"no-locks", 'k', 0, G_OPTION_ARG_NONE, &no_locks,
     "Do not execute the temporary shared read lock.  WARNING: This will cause "
     "inconsistent backups",
     NULL},
    {"all-tablespaces", 'Y', 0 , G_OPTION_ARG_NONE, &dump_tablespaces,
    "Dump all the tablespaces.", NULL},
    {"no-backup-locks", 0, 0, G_OPTION_ARG_NONE, &no_backup_locks,
     "Do not use Percona backup locks", NULL},
    {"lock-all-tables", 0, 0, G_OPTION_ARG_NONE, &lock_all_tables,
     "Use LOCK TABLE for all, instead of FTWRL", NULL},
    {"less-locking", 0, 0, G_OPTION_ARG_NONE, &less_locking,
     "Minimize locking time on InnoDB tables.", NULL},
    {"trx-consistency-only", 0, 0, G_OPTION_ARG_NONE, &trx_consistency_only,
     "Transactional consistency only", NULL},
    {"no-schemas", 'm', 0, G_OPTION_ARG_NONE, &no_schemas,
      "Do not dump table schemas with the data and triggers", NULL},
    {"kill-long-queries", 'K', 0, G_OPTION_ARG_NONE, &killqueries,
     "Kill long running queries (instead of aborting)", NULL},
    { "set-names",0, 0, G_OPTION_ARG_STRING, &set_names_str,
      "Sets the names, use it at your own risk, default binary", NULL },
    { "pmm-path", 0, 0, G_OPTION_ARG_STRING, &pmm_path,
      "which default value will be /usr/local/percona/pmm2/collectors/textfile-collector/high-resolution", NULL },
    { "pmm-resolution", 0, 0, G_OPTION_ARG_STRING, &pmm_resolution,
      "which default will be high", NULL },
    {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}};

void load_start_dump_entries(GOptionGroup *main_group){
  load_dump_into_file_entries(main_group);
  load_working_thread_entries(main_group);
  g_option_group_add_entries(main_group, start_dump_entries);
}


void initialize_start_dump(){
  initialize_common();
  initialize_working_thread();
  all_anonymized_function=g_hash_table_new ( g_str_hash, g_str_equal );
  all_where_per_table=g_hash_table_new ( g_str_hash, g_str_equal );

  if (set_names_str){
    if (strlen(set_names_str)!=0){
      gchar *tmp_str=g_strdup_printf("/*!40101 SET NAMES %s*/",set_names_str);
      set_names_str=tmp_str;
    }else
      set_names_str=NULL;
  } else
    set_names_str=g_strdup("/*!40101 SET NAMES binary*/");  

  // until we have an unique option on lock types we need to ensure this
  if (no_locks || trx_consistency_only)
    less_locking = 0;

  // clarify binlog coordinates with trx_consistency_only
  if (trx_consistency_only)
    g_warning("Using trx_consistency_only, binlog coordinates will not be "
              "accurate if you are writing to non transactional tables.");

  if (db){
    db_items=g_strsplit(db,",",0);
  }

  if (pmm_path){
    pmm=TRUE;
    if (!pmm_resolution){
      pmm_resolution=g_strdup("high");
    }
  }else if (pmm_resolution){
    pmm=TRUE;
    pmm_path=g_strdup_printf("/usr/local/percona/pmm2/collectors/textfile-collector/%s-resolution",pmm_resolution);
  }

  if (stream && exec_command != NULL){
    g_critical("Stream and execute a command is not supported");
    exit(EXIT_FAILURE);
  }
}

/* Write some stuff we know about snapshot, before it changes */
void write_snapshot_info(MYSQL *conn, FILE *file) {
  MYSQL_RES *master = NULL, *slave = NULL, *mdb = NULL;
  MYSQL_FIELD *fields;
  MYSQL_ROW row;

  char *masterlog = NULL;
  char *masterpos = NULL;
  char *mastergtid = NULL;

  char *connname = NULL;
  char *slavehost = NULL;
  char *slavelog = NULL;
  char *slavepos = NULL;
  char *slavegtid = NULL;
  guint isms;
  guint i;

  mysql_query(conn, "SHOW MASTER STATUS");
  master = mysql_store_result(conn);
  if (master && (row = mysql_fetch_row(master))) {
    masterlog = row[0];
    masterpos = row[1];
    /* Oracle/Percona GTID */
    if (mysql_num_fields(master) == 5) {
      mastergtid = row[4];
    } else {
      /* Let's try with MariaDB 10.x */
      /* Use gtid_binlog_pos due to issue with gtid_current_pos with galera
       * cluster, gtid_binlog_pos works as well with normal mariadb server
       * https://jira.mariadb.org/browse/MDEV-10279 */
      mysql_query(conn, "SELECT @@gtid_binlog_pos");
      mdb = mysql_store_result(conn);
      if (mdb && (row = mysql_fetch_row(mdb))) {
        mastergtid = row[0];
      }
    }
  }

  if (masterlog) {
    fprintf(file, "SHOW MASTER STATUS:\n\tLog: %s\n\tPos: %s\n\tGTID:%s\n\n",
            masterlog, masterpos, mastergtid);
    g_message("Written master status");
  }

  isms = 0;
  mysql_query(conn, "SELECT @@default_master_connection");
  MYSQL_RES *rest = mysql_store_result(conn);
  if (rest != NULL && mysql_num_rows(rest)) {
    mysql_free_result(rest);
    g_message("Multisource slave detected.");
    isms = 1;
  }

  if (isms)
    mysql_query(conn, "SHOW ALL SLAVES STATUS");
  else
    mysql_query(conn, "SHOW SLAVE STATUS");

  guint slave_count=0;
  slave = mysql_store_result(conn);
  while (slave && (row = mysql_fetch_row(slave))) {
    fields = mysql_fetch_fields(slave);
    for (i = 0; i < mysql_num_fields(slave); i++) {
      if (isms && !strcasecmp("connection_name", fields[i].name))
        connname = row[i];
      if (!strcasecmp("exec_master_log_pos", fields[i].name)) {
        slavepos = row[i];
      } else if (!strcasecmp("relay_master_log_file", fields[i].name)) {
        slavelog = row[i];
      } else if (!strcasecmp("master_host", fields[i].name)) {
        slavehost = row[i];
      } else if (!strcasecmp("Executed_Gtid_Set", fields[i].name) ||
                 !strcasecmp("Gtid_Slave_Pos", fields[i].name)) {
        slavegtid = row[i];
      }
    }
    if (slavehost) {
      slave_count++;
      fprintf(file, "SHOW SLAVE STATUS:");
      if (isms)
        fprintf(file, "\n\tConnection name: %s", connname);
      fprintf(file, "\n\tHost: %s\n\tLog: %s\n\tPos: %s\n\tGTID:%s\n\n",
              slavehost, slavelog, slavepos, slavegtid);
      g_message("Written slave status");
    }
  }
  if (slave_count > 1)
    g_warning("Multisource replication found. Do not trust in the exec_master_log_pos as it might cause data inconsistencies. Search 'Replication and Transaction Inconsistencies' on MySQL Documentation");

  fflush(file);
  if (master)
    mysql_free_result(master);
  if (slave)
    mysql_free_result(slave);
  if (mdb)
    mysql_free_result(mdb);
}

void set_disk_limits(guint p_at, guint r_at){
  pause_at=p_at;
  resume_at=r_at;
}

gboolean is_disk_space_ok(guint val){
  struct statvfs buffer;
  int ret = statvfs(output_directory, &buffer);
  if (!ret) {
    const double available = (double)(buffer.f_bfree * buffer.f_frsize) / 1024 / 1024;
    return available > val;
  }else{
    g_warning("Disk space check failed");
  }
  return TRUE;
}

void *monitor_disk_space_thread (void *queue){
  (void)queue;
  guint i=0;
  GMutex **pause_mutex_per_thread=g_new(GMutex * , num_threads) ;
  for(i=0;i<num_threads;i++){
    pause_mutex_per_thread[i]=g_mutex_new();
  }

  gboolean previous_state = TRUE, current_state = TRUE;

  while (disk_limits != NULL){
    current_state = previous_state ? is_disk_space_ok(pause_at) : is_disk_space_ok(resume_at);
    if (previous_state != current_state){
      if (!current_state){
        g_warning("Pausing backup disk space lower than %dMB. You need to free up to %dMB to resume",pause_at,resume_at);
        for(i=0;i<num_threads;i++){
          g_mutex_lock(pause_mutex_per_thread[i]);
          g_async_queue_push(queue,pause_mutex_per_thread[i]);
        }
      }else{
        g_warning("Resuming backup");
        for(i=0;i<num_threads;i++){
          g_mutex_unlock(pause_mutex_per_thread[i]);
        }
      }
      previous_state = current_state;

    }
    sleep(10);
  }
  return NULL;
}

GMutex **pause_mutex_per_thread=NULL;

gboolean sig_triggered(void * user_data, int signal) {
  if (signal == SIGTERM){
    shutdown_triggered = TRUE;
  }else{

    guint i=0;
    if (pause_mutex_per_thread == NULL){
      pause_mutex_per_thread=g_new(GMutex * , num_threads) ;
      for(i=0;i<num_threads;i++){
        pause_mutex_per_thread[i]=g_mutex_new();
      }
    }
    if (((struct configuration *)user_data)->pause_resume == NULL)
      ((struct configuration *)user_data)->pause_resume = g_async_queue_new();
    GAsyncQueue *queue = ((struct configuration *)user_data)->pause_resume;
    if (!daemon_mode){
      fprintf(stdout, "Ctrl+c detected! Are you sure you want to cancel(Y/N)?");
      for(i=0;i<num_threads;i++){
        g_mutex_lock(pause_mutex_per_thread[i]);
        g_async_queue_push(queue,pause_mutex_per_thread[i]);
      }
      int c=0;
      while (1){
        do{
          c=fgetc(stdin);
        }while (c=='\n');
        if ( c == 'N' || c == 'n'){
          for(i=0;i<num_threads;i++)
            g_mutex_unlock(pause_mutex_per_thread[i]);
          return TRUE;
        }
        if ( c == 'Y' || c == 'y'){
          shutdown_triggered = TRUE;
          for(i=0;i<num_threads;i++)
            g_mutex_unlock(pause_mutex_per_thread[i]);
          goto finish;
        }
      }
    }
  }
finish:
  g_message("Shutting down gracefully");
  return FALSE;
}

gboolean sig_triggered_int(void * user_data) {
  return sig_triggered(user_data,SIGINT);
}
gboolean sig_triggered_term(void * user_data) {
  return sig_triggered(user_data,SIGTERM);
}

void *signal_thread(void *data) {
  GMainLoop * loop=NULL;
  g_unix_signal_add(SIGINT, sig_triggered_int, data);
  g_unix_signal_add(SIGTERM, sig_triggered_term, data);
  loop = g_main_loop_new (NULL, TRUE);
  g_main_loop_run (loop);
  g_message("Ending signal thread");
  return NULL;
}


GHashTable * mydumper_initialize_hash_of_session_variables(){
  GHashTable * set_session_hash=initialize_hash_of_session_variables();
  g_hash_table_insert(set_session_hash,g_strdup("information_schema_stats_expiry"),g_strdup("0 /*!80003"));
  return set_session_hash;
}

MYSQL *create_main_connection() {
  MYSQL *conn;
  conn = mysql_init(NULL);

  m_connect(conn, "mydumper",db_items!=NULL?db_items[0]:db);

  set_session = g_string_new(NULL);
  detected_server = detect_server(conn);
  GHashTable * set_session_hash = mydumper_initialize_hash_of_session_variables();
  if (key_file != NULL ){
    load_session_hash_from_key_file(key_file,set_session_hash,"mydumper_variables");
    load_where_per_table_and_anonymized_functions_from_key_file(key_file, all_where_per_table, all_anonymized_function, &get_function_pointer_for);
  }
  refresh_set_session_from_hash(set_session,set_session_hash);
  execute_gstring(conn, set_session);

  switch (detected_server) {
  case SERVER_TYPE_MYSQL:
    g_message("Connected to a MySQL server");
    set_transaction_isolation_level_repeatable_read(conn);
    break;
  case SERVER_TYPE_DRIZZLE:
    g_message("Connected to a Drizzle server");
    break;
  case SERVER_TYPE_TIDB:
    g_message("Connected to a TiDB server");
    break;
  default:
    g_critical("Cannot detect server type");
    exit(EXIT_FAILURE);
    break;
  }

  return conn;
}

void get_not_updated(MYSQL *conn, FILE *file) {
  MYSQL_RES *res = NULL;
  MYSQL_ROW row;

  gchar *query =
      g_strdup_printf("SELECT CONCAT(TABLE_SCHEMA,'.',TABLE_NAME) FROM "
                      "information_schema.TABLES WHERE TABLE_TYPE = 'BASE "
                      "TABLE' AND UPDATE_TIME < NOW() - INTERVAL %d DAY",
                      updated_since);
  mysql_query(conn, query);
  g_free(query);

  res = mysql_store_result(conn);
  while ((row = mysql_fetch_row(res))) {
    no_updated_tables = g_list_prepend(no_updated_tables, row[0]);
    fprintf(file, "%s\n", row[0]);
  }
  no_updated_tables = g_list_reverse(no_updated_tables);
  fflush(file);
}

void get_table_info_to_process_from_list(MYSQL *conn, struct configuration *conf, gchar ** table_list) {

  gchar **dt = NULL;
  char *query = NULL;
  guint x;

  for (x = 0; table_list[x] != NULL; x++) {
    dt = g_strsplit(table_list[x], ".", 0);

    query =
        g_strdup_printf("SHOW TABLE STATUS FROM %s LIKE '%s'", dt[0], dt[1]);

    if (mysql_query(conn, (query))) {
      g_critical("Error showing table status on: %s - Could not execute query: %s", dt[0],
                 mysql_error(conn));
      errors++;
      return;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    guint ecol = -1, ccol = -1;
    determine_ecol_ccol(result, &ecol, &ccol);

    struct database * database=NULL;
    if (get_database(conn, dt[0], &database)){
      if (!database->already_dumped){
        g_mutex_lock(database->ad_mutex);
        if (!database->already_dumped){
          create_job_to_dump_schema(database->name, conf);
          database->already_dumped=TRUE;
        }
        g_mutex_unlock(database->ad_mutex);
//        g_async_queue_push(conf->ready_database_dump, GINT_TO_POINTER(1));
      }
    }

    if (!result) {
      g_critical("Could not list tables for %s: %s", database->name, mysql_error(conn));
      errors++;
      return;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {

      int is_view = 0;

      if ((detected_server == SERVER_TYPE_MYSQL) &&
          (row[ccol] == NULL || !strcmp(row[ccol], "VIEW")))
        is_view = 1;

      /* Checks skip list on 'database.table' string */
      if (tables_skiplist_file && check_skiplist(database->name, row[0]))
        continue;

      /* Checks PCRE expressions on 'database.table' string */
      if (!eval_regex(database->name, row[0]))
        continue;

      new_table_to_dump(conn, conf, is_view, database, row[0], row[6], row[ecol]);
    }
  }

  g_free(query);
}

void long_query_wait(MYSQL *conn){
  char *p3=NULL;
    while (TRUE) {
      int longquery_count = 0;
      if (mysql_query(conn, "SHOW PROCESSLIST")) {
        g_warning("Could not check PROCESSLIST, no long query guard enabled: %s",
                  mysql_error(conn));
        break;
      } else {
       MYSQL_RES *res = mysql_store_result(conn);
        MYSQL_ROW row;

        /* Just in case PROCESSLIST output column order changes */
        MYSQL_FIELD *fields = mysql_fetch_fields(res);
        guint i;
        int tcol = -1, ccol = -1, icol = -1, ucol = -1;
        for (i = 0; i < mysql_num_fields(res); i++) {
        if (!strcasecmp(fields[i].name, "Command"))
            ccol = i;
          else if (!strcasecmp(fields[i].name, "Time"))
            tcol = i;
          else if (!strcasecmp(fields[i].name, "Id"))
            icol = i;
          else if (!strcasecmp(fields[i].name, "User"))
            ucol = i;
        }
        if ((tcol < 0) || (ccol < 0) || (icol < 0)) {
          g_critical("Error obtaining information from processlist");
          exit(EXIT_FAILURE);
        }
        while ((row = mysql_fetch_row(res))) {
          if (row[ccol] && strcmp(row[ccol], "Query"))
            continue;
          if (row[ucol] && !strcmp(row[ucol], "system user"))
            continue;
          if (row[tcol] && atoi(row[tcol]) > longquery) {
            if (killqueries) {
              if (mysql_query(conn,
                              p3 = g_strdup_printf("KILL %lu", atol(row[icol])))) {
                g_warning("Could not KILL slow query: %s", mysql_error(conn));
                longquery_count++;
              } else {
                g_warning("Killed a query that was running for %ss", row[tcol]);
              }
              g_free(p3);
            } else {
              longquery_count++;
            }
          }
        }
        mysql_free_result(res);
        if (longquery_count == 0)
          break;
        else {
          if (longquery_retries == 0) {
            g_critical("There are queries in PROCESSLIST running longer than "
                       "%us, aborting dump,\n\t"
                       "use --long-query-guard to change the guard value, kill "
                       "queries (--kill-long-queries) or use \n\tdifferent "
                       "server for dump",
                       longquery);
            exit(EXIT_FAILURE);
          }
          longquery_retries--;
          g_warning("There are queries in PROCESSLIST running longer than "
                         "%us, retrying in %u seconds (%u left).",
                         longquery, longquery_retry_interval, longquery_retries);
          sleep(longquery_retry_interval);
        }
      }
    }
}

void send_mariadb_backup_locks(MYSQL *conn){
  if (mysql_query(conn, "BACKUP STAGE START")) {
    g_critical("Couldn't acquire BACKUP STAGE START: %s",
               mysql_error(conn));
    errors++;
    exit(EXIT_FAILURE);
  }

  if (mysql_query(conn, "BACKUP STAGE FLUSH")) {
    g_critical("Couldn't acquire BACKUP STAGE FLUSH: %s",
               mysql_error(conn));
    errors++;
    exit(EXIT_FAILURE);
  }
  if (mysql_query(conn, "BACKUP STAGE BLOCK_DDL")) {
    g_critical("Couldn't acquire BACKUP STAGE BLOCK_DDL: %s",
               mysql_error(conn));
    errors++;
    exit(EXIT_FAILURE);
  }

  if (mysql_query(conn, "BACKUP STAGE BLOCK_COMMIT")) {
    g_critical("Couldn't acquire BACKUP STAGE BLOCK_COMMIT: %s",
               mysql_error(conn));
    errors++;
    exit(EXIT_FAILURE);
  }
}

void send_percona57_backup_locks(MYSQL *conn){
  if (mysql_query(conn, "LOCK TABLES FOR BACKUP")) {
    g_critical("Couldn't acquire LOCK TABLES FOR BACKUP, snapshots will "
               "not be consistent: %s",
               mysql_error(conn));
    errors++;
    exit(EXIT_FAILURE);
  }

  if (mysql_query(conn, "LOCK BINLOG FOR BACKUP")) {
    g_critical("Couldn't acquire LOCK BINLOG FOR BACKUP, snapshots will "
               "not be consistent: %s",
               mysql_error(conn));
    errors++;
    exit(EXIT_FAILURE);
  }
}

void send_lock_instance_backup(MYSQL *conn){
  if (mysql_query(conn, "LOCK INSTANCE FOR BACKUP")) {
    g_critical("Couldn't acquire LOCK INSTANCE FOR BACKUP: %s",
               mysql_error(conn));
    errors++;
    exit(EXIT_FAILURE);
  }
} 

void send_unlock_tables(MYSQL *conn){
  mysql_query(conn, "UNLOCK TABLES");
}

void send_unlock_binlogs(MYSQL *conn){
  mysql_query(conn, "UNLOCK BINLOG");
}

void send_unlock_instance_backup(MYSQL *conn){
  mysql_query(conn, "UNLOCK INSTANCE");
}

void send_backup_stage_end(MYSQL *conn){
  mysql_query(conn, "BACKUP STAGE END");
}

void determine_ddl_lock_function(MYSQL ** conn, void (**acquire_lock_function)(MYSQL *), void (** release_lock_function)(MYSQL *), void (** release_binlog_function)(MYSQL *)) {
  mysql_query(*conn, "SELECT @@version_comment, @@version");
  MYSQL_RES *res2 = mysql_store_result(*conn);
  MYSQL_ROW ver;
  while ((ver = mysql_fetch_row(res2))) {
    if (g_str_has_prefix(ver[0], "Percona")){
      if (g_str_has_prefix(ver[1], "8.")) {
        *acquire_lock_function = &send_lock_instance_backup;
        *release_lock_function = &send_unlock_instance_backup;
        break;
      }
      if (g_str_has_prefix(ver[1], "5.7.")) {
        *acquire_lock_function = &send_percona57_backup_locks;
        *release_binlog_function = &send_unlock_binlogs;
        *release_lock_function = &send_unlock_tables;
        *conn = create_main_connection();
        break;
      }
    }
    if (g_str_has_prefix(ver[0], "MySQL")){
      if (g_str_has_prefix(ver[1], "8.")) {
        *acquire_lock_function = &send_lock_instance_backup;
        *release_lock_function = &send_unlock_instance_backup;
        break;
      }
    }
    if (g_str_has_prefix(ver[0], "mariadb")){
      if ((g_str_has_prefix(ver[1], "10.5")) || 
          (g_str_has_prefix(ver[1], "10.6"))) {
        *acquire_lock_function = &send_mariadb_backup_locks;
        *release_lock_function = &send_backup_stage_end;
        break;
      }
    }
  }
  mysql_free_result(res2);
}


void send_lock_all_tables(MYSQL *conn){
  // LOCK ALL TABLES
  GString *query = g_string_sized_new(16777216);
  gchar *dbtb = NULL;
  gchar **dt = NULL;
  GList *tables_lock = NULL;
  GList *iter = NULL;
  guint success = 0;
  guint retry = 0;
  guint lock = 1;
  guint i = 0;

  if (tables) {
    for (i = 0; tables[i] != NULL; i++) {
      dt = g_strsplit(tables[i], ".", 0);
      if (tables_skiplist_file && check_skiplist(dt[0], dt[1]))
        continue;
      if (!eval_regex(dt[0], dt[1]))
        continue;
      dbtb = g_strdup_printf("`%s`.`%s`", dt[0], dt[1]);
      tables_lock = g_list_prepend(tables_lock, dbtb);
    }
    tables_lock = g_list_reverse(tables_lock);
  }else{
    if (db) {
      GString *db_quoted_list=NULL;
      db_quoted_list=g_string_sized_new(strlen(db));
      g_string_append_printf(db_quoted_list,"'%s'",db_items[i]);
      i++;
      while (i<g_strv_length(db_items)){
        g_string_append_printf(db_quoted_list,",'%s'",db_items[i]);
        i++;
      }

      g_string_printf(
            query,
            "SELECT TABLE_SCHEMA, TABLE_NAME FROM information_schema.TABLES "
            "WHERE TABLE_SCHEMA in (%s) AND TABLE_TYPE ='BASE TABLE' AND NOT "
            "(TABLE_SCHEMA = 'mysql' AND (TABLE_NAME = 'slow_log' OR "
            "TABLE_NAME = 'general_log'))",
            db_quoted_list->str);
    } else {
      g_string_printf(
        query,
        "SELECT TABLE_SCHEMA, TABLE_NAME FROM information_schema.TABLES "
        "WHERE TABLE_TYPE ='BASE TABLE' AND TABLE_SCHEMA NOT IN "
        "('information_schema', 'performance_schema', 'data_dictionary') "
        "AND NOT (TABLE_SCHEMA = 'mysql' AND (TABLE_NAME = 'slow_log' OR "
        "TABLE_NAME = 'general_log'))");
    }
  }

  if (tables_lock == NULL && query->len > 0  ) {
    if (mysql_query(conn, query->str)) {
      g_critical("Couldn't get table list for lock all tables: %s",
                 mysql_error(conn));
      errors++;
    } else {
      MYSQL_RES *res = mysql_store_result(conn);
      MYSQL_ROW row;

      while ((row = mysql_fetch_row(res))) {
        lock = 1;
        if (tables) {
          int table_found = 0;
          for (i = 0; tables[i] != NULL; i++)
            if (g_ascii_strcasecmp(tables[i], row[1]) == 0)
              table_found = 1;
          if (!table_found)
              lock = 0;
        }
        if (lock && tables_skiplist_file && check_skiplist(row[0], row[1]))
          continue;
        if (lock && !eval_regex(row[0], row[1]))
          continue;
        if (lock) {
          dbtb = g_strdup_printf("`%s`.`%s`", row[0], row[1]);
          tables_lock = g_list_prepend(tables_lock, dbtb);
        }
      }
      tables_lock = g_list_reverse(tables_lock);
    }
  }
  if (tables_lock != NULL) {
  // Try three times to get the lock, this is in case of tmp tables
  // disappearing
    while (!success && retry < 4) {
      g_string_set_size(query,0);
      g_string_append(query, "LOCK TABLE");
      for (iter = tables_lock; iter != NULL; iter = iter->next) {
        g_string_append_printf(query, "%s READ,", (char *)iter->data);
      }
      g_strrstr(query->str,",")[0]=' ';

      if (mysql_query(conn, query->str)) {
        gchar *failed_table = NULL;
        gchar **tmp_fail;

        tmp_fail = g_strsplit(mysql_error(conn), "'", 0);
        tmp_fail = g_strsplit(tmp_fail[1], ".", 0);
        failed_table = g_strdup_printf("`%s`.`%s`", tmp_fail[0], tmp_fail[1]);
        for (iter = tables_lock; iter != NULL; iter = iter->next) {
          if (strcmp(iter->data, failed_table) == 0) {
            tables_lock = g_list_remove(tables_lock, iter->data);
          }
        }
        g_free(tmp_fail);
        g_free(failed_table);
      } else {
        success = 1;
      }
      retry += 1;
    }
    if (!success) {
      g_critical("Lock all tables fail: %s", mysql_error(conn));
      exit(EXIT_FAILURE);
    }
  }else{
    g_critical("No table found to lock");
    exit(EXIT_FAILURE);
  }
  g_free(query->str);
  g_list_free(tables_lock);
}

void start_dump() {
  MYSQL *conn = create_main_connection();
  MYSQL *second_conn = conn;
  struct configuration conf = {1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0};
  char *metadata_partial_filename, *metadata_filename;
  char *u;
  detect_server_version(conn);
  void (*acquire_ddl_lock_function)(MYSQL *) = NULL;
  void (*release_ddl_lock_function)(MYSQL *) = NULL;
  void (*release_binlog_function)(MYSQL *) = NULL;

  guint64 nits[num_threads];
  GList *nitl[num_threads];
  int tn = 0;
  guint64 min = 0;
  struct db_table *dbt=NULL;
//  struct schema_post *sp;
  guint n;
  FILE *nufile = NULL;
  GThread *disk_check_thread = NULL;
  if (disk_limits!=NULL){
    conf.pause_resume = g_async_queue_new();
    disk_check_thread = g_thread_create(monitor_disk_space_thread, conf.pause_resume, FALSE, NULL);
  }

  if (!daemon_mode){
    GError *serror;
    GThread *sthread =
        g_thread_create(signal_thread, &conf, FALSE, &serror);
    if (sthread == NULL) {
      g_critical("Could not create signal thread: %s", serror->message);
      g_error_free(serror);
      exit(EXIT_FAILURE);
    }
  }


  GThread *pmmthread = NULL;
  if (pmm){

    g_message("Using PMM resolution %s at %s", pmm_resolution, pmm_path);
    GError *serror;
    pmmthread =
        g_thread_create(pmm_thread, &conf, FALSE, &serror);
    if (pmmthread == NULL) {
      g_critical("Could not create pmm thread: %s", serror->message);
      g_error_free(serror);
      exit(EXIT_FAILURE);
    }
  }

  for (n = 0; n < num_threads; n++) {
    nits[n] = 0;
    nitl[n] = NULL;
  }

  metadata_partial_filename = g_strdup_printf("%s/metadata.partial", dump_directory);
  metadata_filename = g_strndup(metadata_partial_filename, (unsigned)strlen(metadata_partial_filename) - 8);

  FILE *mdfile = g_fopen(metadata_partial_filename, "w");
  if (!mdfile) {
    g_critical("Couldn't write metadata file %s (%d)", metadata_partial_filename, errno);
    exit(EXIT_FAILURE);
  }

  if (updated_since > 0) {
    u = g_strdup_printf("%s/not_updated_tables", dump_directory);
    nufile = g_fopen(u, "w");
    if (!nufile) {
      g_critical("Couldn't write not_updated_tables file (%d)", errno);
      exit(EXIT_FAILURE);
    }
    get_not_updated(conn, nufile);
  }

  if (!no_locks) {
  // We check SHOW PROCESSLIST, and if there're queries
  // larger than preset value, we terminate the process.
  // This avoids stalling whole server with flush.
		long_query_wait(conn);
	}

  if (detected_server == SERVER_TYPE_TIDB) {
    g_message("Skipping locks because of TiDB");
    if (!tidb_snapshot) {

      // Generate a @@tidb_snapshot to use for the worker threads since
      // the tidb-snapshot argument was not specified when starting mydumper

      if (mysql_query(conn, "SHOW MASTER STATUS")) {
        g_critical("Couldn't generate @@tidb_snapshot: %s", mysql_error(conn));
        exit(EXIT_FAILURE);
      } else {

        MYSQL_RES *result = mysql_store_result(conn);
        MYSQL_ROW row = mysql_fetch_row(
            result); /* There should never be more than one row */
        tidb_snapshot = g_strdup(row[1]);
        mysql_free_result(result);
      }
    }

    // Need to set the @@tidb_snapshot for the master thread
    gchar *query =
        g_strdup_printf("SET SESSION tidb_snapshot = '%s'", tidb_snapshot);

    g_message("Set to tidb_snapshot '%s'", tidb_snapshot);

    if (mysql_query(conn, query)) {
      g_critical("Failed to set tidb_snapshot: %s", mysql_error(conn));
      exit(EXIT_FAILURE);
    }
    g_free(query);

  }else{

    if (!no_locks) {
      // This backup will lock the database
      if (!no_backup_locks)
        determine_ddl_lock_function(&second_conn,&acquire_ddl_lock_function,&release_ddl_lock_function, &release_binlog_function);

      if (lock_all_tables) {
        send_lock_all_tables(conn);
      } else {
        g_message("Sending Flush Table");
        if (mysql_query(conn, "FLUSH NO_WRITE_TO_BINLOG TABLES")) {
          g_warning("Flush tables failed, we are continuing anyways: %s",
                   mysql_error(conn));
        }
        g_message("Acquiring FTWRL");
        if (mysql_query(conn, "FLUSH TABLES WITH READ LOCK")) {
          g_critical("Couldn't acquire global lock, snapshots will not be "
                   "consistent: %s",
                   mysql_error(conn));
          errors++;
        }
        if (acquire_ddl_lock_function != NULL) {
          g_message("Acquiring DDL lock");
          acquire_ddl_lock_function(second_conn);
        }
      }
    } else {
      g_warning("Executing in no-locks mode, snapshot might not be consistent");
    }
  }


// TODO: this should be deleted on future releases. 
  if (mysql_get_server_version(conn) < 40108) {
    mysql_query(
        conn,
        "CREATE TABLE IF NOT EXISTS mysql.mydumperdummy (a INT) ENGINE=INNODB");
    need_dummy_read = 1;
  }

  // tokudb do not support consistent snapshot
  mysql_query(conn, "SELECT @@tokudb_version");
  MYSQL_RES *rest = mysql_store_result(conn);
  if (rest != NULL && mysql_num_rows(rest)) {
    mysql_free_result(rest);
    g_message("TokuDB detected, creating dummy table for CS");
    mysql_query(
        conn,
        "CREATE TABLE IF NOT EXISTS mysql.tokudbdummy (a INT) ENGINE=TokuDB");
    need_dummy_toku_read = 1;
  }

  // Do not start a transaction when lock all tables instead of FTWRL,
  // since it can implicitly release read locks we hold
  // TODO: this should be deleted as main connection is not being used for export data
  if (!lock_all_tables) {
    g_message("Sending start transaction in main connection");
    mysql_query(conn, "START TRANSACTION /*!40108 WITH CONSISTENT SNAPSHOT */");
  }

  if (need_dummy_read) {
    mysql_query(conn,
                "SELECT /*!40001 SQL_NO_CACHE */ * FROM mysql.mydumperdummy");
    MYSQL_RES *res = mysql_store_result(conn);
    if (res)
      mysql_free_result(res);
  }
  if (need_dummy_toku_read) {
    mysql_query(conn,
                "SELECT /*!40001 SQL_NO_CACHE */ * FROM mysql.tokudbdummy");
    MYSQL_RES *res = mysql_store_result(conn);
    if (res)
      mysql_free_result(res);
  }

  GDateTime *datetime = g_date_time_new_now_local();
  char *datetimestr=g_date_time_format(datetime,"\%Y-\%m-\%d \%H:\%M:\%S");
  fprintf(mdfile, "Started dump at: %s\n", datetimestr);
  g_message("Started dump at: %s", datetimestr);
  g_free(datetimestr);

  if (detected_server == SERVER_TYPE_MYSQL) {
    if (set_names_str)
  		mysql_query(conn, set_names_str);
    write_snapshot_info(conn, mdfile);
  }


  if (stream){
    initialize_stream();
  }

  if (exec_command != NULL){
    initialize_exec_command();
    stream=TRUE;
  
  }

  GThread **threads = g_new(GThread *, num_threads * (less_locking + 1));
  struct thread_data *td =
      g_new(struct thread_data, num_threads * (less_locking + 1));

  if (less_locking) {
    conf.queue_less_locking = g_async_queue_new();
    conf.ready_less_locking = g_async_queue_new();
    for (n = num_threads; n < num_threads * 2; n++) {
      td[n].conf = &conf;
      td[n].thread_id = n + 1;
      td[n].queue = conf.queue_less_locking;
      td[n].ready = conf.ready_less_locking;
      td[n].less_locking_stage = TRUE;
      td[n].binlog_snapshot_gtid_executed = NULL;
      threads[n] = g_thread_create((GThreadFunc)working_thread,
                                   &td[n], TRUE, NULL);
      g_async_queue_pop(conf.ready_less_locking);
    }
    g_async_queue_unref(conf.ready_less_locking);
    conf.ready_less_locking=NULL;
  }

  conf.queue = g_async_queue_new();
  conf.ready = g_async_queue_new();
  conf.unlock_tables = g_async_queue_new();
  ready_database_dump_mutex = g_mutex_new();
  g_mutex_lock(ready_database_dump_mutex);
//  conf.ready_database_dump = g_async_queue_new();

  for (n = 0; n < num_threads; n++) {
    td[n].conf = &conf;
    td[n].thread_id = n + 1;
    td[n].queue = conf.queue;
    td[n].ready = conf.ready;
    td[n].less_locking_stage = FALSE;
    td[n].binlog_snapshot_gtid_executed = NULL;
    threads[n] =
        g_thread_create((GThreadFunc)working_thread, &td[n], TRUE, NULL);
 //   g_async_queue_pop(conf.ready);
  }

  for (n = 0; n < num_threads; n++) {
    g_async_queue_pop(conf.ready);
  }


  // IMPORTANT: At this point, all the threads are in sync

  g_async_queue_unref(conf.ready);
  conf.ready=NULL;

  if (trx_consistency_only) {
    g_message("Transactions started, unlocking tables");
    mysql_query(conn, "UNLOCK TABLES /* trx-only */");
    if (release_binlog_function != NULL){
      g_message("Releasing binlog lock");
      release_binlog_function(second_conn);
    }
  }
  if (dump_tablespaces){
    create_job_to_dump_tablespaces(conn,&conf);
  }

  if (db) {
    guint i=0;
    for (i=0;i<g_strv_length(db_items);i++){
      create_job_to_dump_database(new_database(conn,db_items[i],TRUE), &conf, less_locking);
      if (!no_schemas)
        create_job_to_dump_schema(db_items[i], &conf);
    }
  } 
  if (tables) {
    get_table_info_to_process_from_list(conn, &conf, tables);
  } 
  if (( db == NULL ) && ( tables == NULL )) {
    MYSQL_RES *databases;
    MYSQL_ROW row;
    if (mysql_query(conn, "SHOW DATABASES") ||
        !(databases = mysql_store_result(conn))) {
      g_critical("Unable to list databases: %s", mysql_error(conn));
      exit(EXIT_FAILURE);
    }

    while ((row = mysql_fetch_row(databases))) {
      if (!strcasecmp(row[0], "information_schema") ||
          !strcasecmp(row[0], "performance_schema") ||
          (!strcasecmp(row[0], "data_dictionary")))
        continue;
      struct database * db_tmp=NULL;
      if (get_database(conn,row[0],&db_tmp) && !no_schemas && (eval_regex(row[0], NULL))){
        g_mutex_lock(db_tmp->ad_mutex);
        if (!db_tmp->already_dumped){
          create_job_to_dump_schema(db_tmp->name, &conf);
          db_tmp->already_dumped=TRUE;
        }
        g_mutex_unlock(db_tmp->ad_mutex);
      }
      create_job_to_dump_database(db_tmp, &conf, less_locking);
    }
    mysql_free_result(databases);
  }
  if (database_counter > 0)
    g_mutex_lock(ready_database_dump_mutex);
  g_list_free(no_updated_tables);

  GList *iter;
  non_innodb_table = g_list_reverse(non_innodb_table);
  if (!non_innodb_table) {
    g_async_queue_push(conf.unlock_tables, GINT_TO_POINTER(1));
  }
  if (less_locking) {

    for (iter = non_innodb_table; iter != NULL; iter = iter->next) {
      dbt = (struct db_table *)iter->data;
      tn = 0;
      min = nits[0];
      for (n = 1; n < num_threads; n++) {
        if (nits[n] < min) {
          min = nits[n];
          tn = n;
        }
      }
      nitl[tn] = g_list_prepend(nitl[tn], dbt);
      nits[tn] += dbt->datalength;
    }
    nitl[tn] = g_list_reverse(nitl[tn]);

    for (n = 0; n < num_threads; n++) {
      if (nits[n] > 0) {
        g_atomic_int_inc(&non_innodb_table_counter);
        create_jobs_for_non_innodb_table_list_in_less_locking_mode(conn, nitl[n], &conf);
        g_list_free(nitl[n]);
      }
    }
    g_list_free(non_innodb_table);

    if (g_atomic_int_get(&non_innodb_table_counter))
      g_atomic_int_inc(&non_innodb_done);
    else
      g_async_queue_push(conf.unlock_tables, GINT_TO_POINTER(1));
    g_message("Shutdown jobs for less locking enqueued");
    for (n = 0; n < num_threads; n++) {
      struct job *j = g_new0(struct job, 1);
      j->type = JOB_SHUTDOWN;
      g_async_queue_push(conf.queue_less_locking, j);
    }
  } else {
    for (iter = non_innodb_table; iter != NULL; iter = iter->next) {
      dbt = (struct db_table *)iter->data;
      create_job_to_dump_table(conn, dbt, &conf, FALSE);
      g_atomic_int_inc(&non_innodb_table_counter);
    }
    g_list_free(non_innodb_table);
    g_atomic_int_inc(&non_innodb_done);
  }

  if (less_locking) {
    g_message("Waiting less locking jobs to complete");
    for (n = num_threads; n < num_threads * 2; n++) {
      g_thread_join(threads[n]);
    }
    g_async_queue_unref(conf.queue_less_locking);
    conf.queue_less_locking=NULL;
  }

  if (!no_locks && !trx_consistency_only) {
    g_async_queue_pop(conf.unlock_tables);
    g_message("Non-InnoDB dump complete, unlocking tables");
    mysql_query(conn, "UNLOCK TABLES /* FTWRL */");
    g_message("Releasing DDL lock");
    if (release_binlog_function != NULL){
      g_message("Releasing binlog lock");
      release_binlog_function(second_conn);
    }
  }

  g_message("Shutdown jobs enqueued");
  for (n = 0; n < num_threads; n++) {
    struct job *j = g_new0(struct job, 1);
    j->type = JOB_SHUTDOWN;
    g_async_queue_push(conf.queue, j);
  }

  g_message("Waiting jobs to complete");
  for (n = 0; n < num_threads; n++) {
    g_thread_join(threads[n]);
  }

  if (release_ddl_lock_function != NULL) {
    g_message("Releasing DDL lock");
    release_ddl_lock_function(second_conn);
  }
  // close main connection
  mysql_close(conn);
  g_message("Main connection closed");  

  // TODO: We need to create jobs for metadata.
  table_schemas = g_list_reverse(table_schemas);
  for (iter = table_schemas; iter != NULL; iter = iter->next) {
    dbt = (struct db_table *)iter->data;
    write_table_metadata_into_file(dbt);
  }
  g_list_free(table_schemas);
  table_schemas=NULL;
  if (pmm){
    kill_pmm_thread();
//    g_thread_join(pmmthread);
  }
  g_async_queue_unref(conf.queue);
  conf.queue=NULL;
  g_async_queue_unref(conf.unlock_tables);
  conf.unlock_tables=NULL;

  datetime = g_date_time_new_now_local();
  datetimestr=g_date_time_format(datetime,"\%Y-\%m-\%d \%H:\%M:\%S");
  fprintf(mdfile, "Finished dump at: %s\n", datetimestr);
  fclose(mdfile);
  if (updated_since > 0)
    fclose(nufile);
  g_rename(metadata_partial_filename, metadata_filename);
  if (stream) {
    g_async_queue_push(stream_queue, g_strdup(metadata_filename));
  }
  g_free(metadata_partial_filename);
  g_free(metadata_filename);
  g_message("Finished dump at: %s",datetimestr);
  g_free(datetimestr);

  if (stream) {
    g_async_queue_push(stream_queue, g_strdup(""));
    if (exec_command!=NULL){
      wait_exec_command_to_finish();
    }else
      wait_stream_to_finish();
    if (no_delete == FALSE && output_directory_param == NULL)
      if (g_rmdir(output_directory) != 0)
        g_critical("Backup directory not removed: %s", output_directory);
  }

  g_free(td);
  g_free(threads);
  if (disk_check_thread!=NULL){
    disk_limits=NULL;
  }
}

