/*
    event.c

    Generalised event handling for motion

    Copyright Jeroen Vreeken, 2002
    This software is distributed under the GNU Public License Version 2
    see also the file 'COPYING'.
*/
#include "picture.h"   /* already includes motion.h */
#include "netcam.h"
#include "movie.h"
#include "event.h"
#include "video_loopback.h"
#include "video_common.h"

/* Various functions (most doing the actual action)
 * TODO Items:
 * Rework the snprintf uses.
 * Edit directories so they can never be null and eliminate defaults from here
 * Move the movie initialize stuff to movie module
 * eliminate #if for v4l2
 * Eliminate #IF for database items
 * Move database functions out of here.
 * Move stream stuff to webu_stream
 * Use (void) alternative for ATTRIBUTE_UNUSED
 */

const char *eventList[] = {
    "NULL",
    "EVENT_FILECREATE",
    "EVENT_MOTION",
    "EVENT_FIRSTMOTION",
    "EVENT_ENDMOTION",
    "EVENT_STOP",
    "EVENT_TIMELAPSE",
    "EVENT_TIMELAPSEEND",
    "EVENT_STREAM",
    "EVENT_IMAGE_DETECTED",
    "EVENT_IMAGEM_DETECTED",
    "EVENT_IMAGE_SNAPSHOT",
    "EVENT_IMAGE",
    "EVENT_IMAGEM",
    "EVENT_IMAGE_PREVIEW",
    "EVENT_FILECLOSE",
    "EVENT_DEBUG",
    "EVENT_CRITICAL",
    "EVENT_AREA_DETECTED",
    "EVENT_CAMERA_LOST",
    "EVENT_CAMERA_FOUND",
    "EVENT_MOVIE_PUT",
    "EVENT_LAST"
};

/**
 * eventToString
 *
 * returns string label of the event
 */
 /**
 * Future use debug / notification function
static const char *eventToString(motion_event e)
{
    return eventList[(int)e];
}
*/

/**
 * exec_command
 *      Execute 'command' with 'arg' as its argument.
 *      if !arg command is started with no arguments
 *      Before we call execl we need to close all the file handles
 *      that the fork inherited from the parent in order not to pass
 *      the open handles on to the shell
 */
static void exec_command(struct ctx_cam *cam, char *command, char *filename, int filetype)
{
    char stamp[PATH_MAX];
    mystrftime(cam, stamp, sizeof(stamp), command, &cam->current_image->timestamp_tv, filename, filetype);

    if (!fork()) {
        int i;

        /* Detach from parent */
        setsid();

        /*
         * Close any file descriptor except console because we will
         * like to see error messages
         */
        for (i = getdtablesize() - 1; i > 2; i--)
            close(i);

        execl("/bin/sh", "sh", "-c", stamp, " &", NULL);

        /* if above function succeeds the program never reach here */
        MOTION_LOG(ALR, TYPE_EVENTS, SHOW_ERRNO
            ,_("Unable to start external command '%s'"), stamp);

        exit(1);
    }

    MOTION_LOG(DBG, TYPE_EVENTS, NO_ERRNO
        ,_("Executing external command '%s'"), stamp);
}

/*
 * Event handlers
 */

static void event_newfile(struct ctx_cam *cam ATTRIBUTE_UNUSED,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy ATTRIBUTE_UNUSED, char *filename, void *ftype,
            struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO
        ,_("File of type %ld saved to: %s")
        ,(unsigned long)ftype, filename);
}


static void event_beep(struct ctx_cam *cam, motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy ATTRIBUTE_UNUSED,
            char *filename ATTRIBUTE_UNUSED,
            void *ftype ATTRIBUTE_UNUSED,
            struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    if (!cam->conf.quiet)
        printf("\a");
}

/**
 * on_picture_save_command
 *      handles both on_picture_save and on_movie_start
 *      If arg = FTYPE_IMAGE_ANY on_picture_save script is executed
 *      If arg = FTYPE_MPEG_ANY on_movie_start script is executed
 *      The scripts are executed with the filename of picture or movie appended
 *      to the config parameter.
 */
static void on_picture_save_command(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy ATTRIBUTE_UNUSED,
            char *filename, void *arg, struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    int filetype = (unsigned long)arg;

    if ((filetype & FTYPE_IMAGE_ANY) != 0 && cam->conf.on_picture_save)
        exec_command(cam, cam->conf.on_picture_save, filename, filetype);

    if ((filetype & FTYPE_MPEG_ANY) != 0 && cam->conf.on_movie_start)
        exec_command(cam, cam->conf.on_movie_start, filename, filetype);
}

static void on_motion_detected_command(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy1 ATTRIBUTE_UNUSED,
            char *dummy2 ATTRIBUTE_UNUSED, void *dummy3 ATTRIBUTE_UNUSED,
            struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    if (cam->conf.on_motion_detected)
        exec_command(cam, cam->conf.on_motion_detected, NULL, 0);
}

#if defined(HAVE_MYSQL) || defined(HAVE_PGSQL) || defined(HAVE_SQLITE3) || defined(HAVE_MARIADB)

static void do_sql_query(char *sqlquery, struct ctx_cam *cam, int save_id)
{

    if (strlen(sqlquery) <= 0) {
        /* don't try to execute empty queries */
        MOTION_LOG(WRN, TYPE_DB, NO_ERRNO, "Ignoring empty sql query");
        return;
    }

    #if defined(HAVE_MYSQL) || defined(HAVE_MARIADB)
        if (!strcmp(cam->conf.database_type, "mysql")) {
            MOTION_LOG(DBG, TYPE_DB, NO_ERRNO, "Executing mysql query");
            if (mysql_query(cam->database, sqlquery) != 0) {
                int error_code = mysql_errno(cam->database);

                MOTION_LOG(ERR, TYPE_DB, SHOW_ERRNO
                    ,_("Mysql query failed %s error code %d")
                    ,mysql_error(cam->database), error_code);
                /* Try to reconnect ONCE if fails continue and discard this sql query */
                if (error_code >= 2000) {
                    // Close connection before start a new connection
                    mysql_close(cam->database);

                    cam->database = (MYSQL *) mymalloc(sizeof(MYSQL));
                    mysql_init(cam->database);

                    if (!mysql_real_connect(cam->database, cam->conf.database_host,
                                            cam->conf.database_user, cam->conf.database_password,
                                            cam->conf.database_dbname, 0, NULL, 0)) {
                        MOTION_LOG(ALR, TYPE_DB, NO_ERRNO
                            ,_("Cannot reconnect to MySQL"
                            " database %s on host %s with user %s MySQL error was %s"),
                            cam->conf.database_dbname,
                            cam->conf.database_host, cam->conf.database_user,
                            mysql_error(cam->database));
                    } else {
                        MOTION_LOG(INF, TYPE_DB, NO_ERRNO
                            ,_("Re-Connection to Mysql database '%s' Succeed")
                            ,cam->conf.database_dbname);
                        if (mysql_query(cam->database, sqlquery) != 0) {
                            int error_my = mysql_errno(cam->database);
                            MOTION_LOG(ERR, TYPE_DB, SHOW_ERRNO
                                ,_("after re-connection Mysql query failed %s error code %d")
                                ,mysql_error(cam->database), error_my);
                        }
                    }
                }
            }
            if (save_id) {
                cam->database_event_id = (unsigned long long) mysql_insert_id(cam->database);
            }
        }
    #endif /* HAVE_MYSQL HAVE_MARIADB*/


    #ifdef HAVE_PGSQL
        if (!strcmp(cam->conf.database_type, "postgresql")) {
            MOTION_LOG(DBG, TYPE_DB, NO_ERRNO, "Executing postgresql query");
            PGresult *res;

            res = PQexec(cam->database_pg, sqlquery);

            if (PQstatus(cam->database_pg) == CONNECTION_BAD) {

                MOTION_LOG(ERR, TYPE_DB, NO_ERRNO
                    ,_("Connection to PostgreSQL database '%s' failed: %s")
                    ,cam->conf.database_dbname, PQerrorMessage(cam->database_pg));

            // This function will close the connection to the server and attempt to reestablish a new connection to the same server,
            // using all the same parameters previously used. This may be useful for error recovery if a working connection is lost
                PQreset(cam->database_pg);

                if (PQstatus(cam->database_pg) == CONNECTION_BAD) {
                    MOTION_LOG(ERR, TYPE_DB, NO_ERRNO
                        ,_("Re-Connection to PostgreSQL database '%s' failed: %s")
                        ,cam->conf.database_dbname, PQerrorMessage(cam->database_pg));
                } else {
                    MOTION_LOG(INF, TYPE_DB, NO_ERRNO
                        ,_("Re-Connection to PostgreSQL database '%s' Succeed")
                        ,cam->conf.database_dbname);
                }

            } else if (!(PQresultStatus(res) == PGRES_COMMAND_OK || PQresultStatus(res) == PGRES_TUPLES_OK)) {
                MOTION_LOG(ERR, TYPE_DB, SHOW_ERRNO, "PGSQL query failed: [%s]  %s %s",
                        sqlquery, PQresStatus(PQresultStatus(res)), PQresultErrorMessage(res));
            }
            if (save_id) {
                //ToDO:  Find the equivalent option for pgsql
                cam->database_event_id = 0;
            }

            PQclear(res);
        }
    #endif /* HAVE_PGSQL */

    #ifdef HAVE_SQLITE3
        if ((!strcmp(cam->conf.database_type, "sqlite3")) && (cam->conf.database_dbname)) {
            int res;
            char *errmsg = 0;
            MOTION_LOG(DBG, TYPE_DB, NO_ERRNO, "Executing sqlite query");
            res = sqlite3_exec(cam->database_sqlite3, sqlquery, NULL, 0, &errmsg);
            if (res != SQLITE_OK ) {
                MOTION_LOG(ERR, TYPE_DB, NO_ERRNO, _("SQLite error was %s"), errmsg);
                sqlite3_free(errmsg);
            }
            if (save_id) {
                //ToDO:  Find the equivalent option for sqlite3
                cam->database_event_id = 0;
            }

        }
    #endif /* HAVE_SQLITE3 */
}

static void event_sqlfirstmotion(struct ctx_cam *cam, motion_event type  ATTRIBUTE_UNUSED,
                                 struct image_data *dummy1 ATTRIBUTE_UNUSED,
                                 char *dummy2 ATTRIBUTE_UNUSED, void *dummy3 ATTRIBUTE_UNUSED,
                                 struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    /* Only log the file types we want */
    if (!(cam->conf.database_type)) {
        return;
    }

    /*
     * We place the code in a block so we only spend time making space in memory
     * for the sqlquery and timestr when we actually need it.
     */
    {
        char sqlquery[PATH_MAX];

        mystrftime(cam, sqlquery, sizeof(sqlquery), cam->conf.sql_query_start,
                   &cam->current_image->timestamp_tv, NULL, 0);

        do_sql_query(sqlquery, cam, 1);
    }
}

static void event_sqlnewfile(struct ctx_cam *cam, motion_event type  ATTRIBUTE_UNUSED,
            struct image_data *dummy ATTRIBUTE_UNUSED,
            char *filename, void *arg, struct timeval *currenttime_tv)
{
    int sqltype = (unsigned long)arg;

    /* Only log the file types we want */
    if (!(cam->conf.database_type) || (sqltype & cam->sql_mask) == 0)
        return;

    /*
     * We place the code in a block so we only spend time making space in memory
     * for the sqlquery and timestr when we actually need it.
     */
    {
        char sqlquery[PATH_MAX];

        mystrftime(cam, sqlquery, sizeof(sqlquery), cam->conf.sql_query,
                   currenttime_tv, filename, sqltype);

        do_sql_query(sqlquery, cam, 0);
    }
}

static void event_sqlfileclose(struct ctx_cam *cam, motion_event type  ATTRIBUTE_UNUSED,
            struct image_data *dummy ATTRIBUTE_UNUSED,
            char *filename, void *arg, struct timeval *currenttime_tv)
{
    int sqltype = (unsigned long)arg;

    /* Only log the file types we want */
    if (!(cam->conf.database_type) || (sqltype & cam->sql_mask) == 0)
        return;

    /*
     * We place the code in a block so we only spend time making space in memory
     * for the sqlquery and timestr when we actually need it.
     */
    {
        char sqlquery[PATH_MAX];

        mystrftime(cam, sqlquery, sizeof(sqlquery), cam->conf.sql_query_stop,
                   currenttime_tv, filename, sqltype);

        do_sql_query(sqlquery, cam, 0);
    }
}

#endif /* defined HAVE_MYSQL || defined HAVE_PGSQL || defined(HAVE_SQLITE3) || defined(HAVE_MARIADB) */

static void on_area_command(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy1 ATTRIBUTE_UNUSED,
            char *dummy2 ATTRIBUTE_UNUSED, void *dummy3 ATTRIBUTE_UNUSED,
            struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    if (cam->conf.on_area_detected)
        exec_command(cam, cam->conf.on_area_detected, NULL, 0);
}

static void on_event_start_command(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy1 ATTRIBUTE_UNUSED,
            char *dummy2 ATTRIBUTE_UNUSED, void *dummy3 ATTRIBUTE_UNUSED,
            struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    if (cam->conf.on_event_start)
        exec_command(cam, cam->conf.on_event_start, NULL, 0);
}

static void on_event_end_command(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy1 ATTRIBUTE_UNUSED,
            char *dummy2 ATTRIBUTE_UNUSED, void *dummy3 ATTRIBUTE_UNUSED,
            struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    if (cam->conf.on_event_end)
        exec_command(cam, cam->conf.on_event_end, NULL, 0);
}

static void event_stream_put(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    int subsize;

    pthread_mutex_lock(&cam->mutex_stream);
        /* Normal stream processing */
        if (cam->stream_norm.cnct_count > 0){
            if (cam->stream_norm.jpeg_data == NULL){
                cam->stream_norm.jpeg_data = mymalloc(cam->imgs.size_norm);
            }
            if (img_data->image_norm != NULL){
                cam->stream_norm.jpeg_size = put_picture_memory(cam
                    ,cam->stream_norm.jpeg_data
                    ,cam->imgs.size_norm
                    ,img_data->image_norm
                    ,cam->conf.stream_quality
                    ,cam->imgs.width
                    ,cam->imgs.height);
            }
        }

        /* Substream processing */
        if (cam->stream_sub.cnct_count > 0){
            if (cam->stream_sub.jpeg_data == NULL){
                cam->stream_sub.jpeg_data = mymalloc(cam->imgs.size_norm);
            }
            if (img_data->image_norm != NULL){
                /* Resulting substream image must be multiple of 8 */
                if (((cam->imgs.width  % 16) == 0)  &&
                    ((cam->imgs.height % 16) == 0)) {

                    subsize = ((cam->imgs.width / 2) * (cam->imgs.height / 2) * 3 / 2);
                    if (cam->imgs.substream_image == NULL){
                        cam->imgs.substream_image = mymalloc(subsize);
                    }
                    pic_scale_img(cam->imgs.width
                        ,cam->imgs.height
                        ,img_data->image_norm
                        ,cam->imgs.substream_image);
                    cam->stream_sub.jpeg_size = put_picture_memory(cam
                        ,cam->stream_sub.jpeg_data
                        ,subsize
                        ,cam->imgs.substream_image
                        ,cam->conf.stream_quality
                        ,(cam->imgs.width / 2)
                        ,(cam->imgs.height / 2));
                } else {
                    /* Substream was not multiple of 8 so send full image*/
                    cam->stream_sub.jpeg_size = put_picture_memory(cam
                        ,cam->stream_sub.jpeg_data
                        ,cam->imgs.size_norm
                        ,img_data->image_norm
                        ,cam->conf.stream_quality
                        ,cam->imgs.width
                        ,cam->imgs.height);
                }
            }
        }

        /* Motion stream processing */
        if (cam->stream_motion.cnct_count > 0){
            if (cam->stream_motion.jpeg_data == NULL){
                cam->stream_motion.jpeg_data = mymalloc(cam->imgs.size_norm);
            }
            if (cam->imgs.img_motion.image_norm != NULL){
                cam->stream_motion.jpeg_size = put_picture_memory(cam
                    ,cam->stream_motion.jpeg_data
                    ,cam->imgs.size_norm
                    ,cam->imgs.img_motion.image_norm
                    ,cam->conf.stream_quality
                    ,cam->imgs.width
                    ,cam->imgs.height);
            }
        }

        /* Source stream processing */
        if (cam->stream_source.cnct_count > 0){
            if (cam->stream_source.jpeg_data == NULL){
                cam->stream_source.jpeg_data = mymalloc(cam->imgs.size_norm);
            }
            if (cam->imgs.image_virgin.image_norm != NULL){
                cam->stream_source.jpeg_size = put_picture_memory(cam
                    ,cam->stream_source.jpeg_data
                    ,cam->imgs.size_norm
                    ,cam->imgs.image_virgin.image_norm
                    ,cam->conf.stream_quality
                    ,cam->imgs.width
                    ,cam->imgs.height);
            }
        }
    pthread_mutex_unlock(&cam->mutex_stream);

}


#if defined(HAVE_V4L2) && !defined(BSD)
static void event_vlp_putpipe(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data, char *dummy ATTRIBUTE_UNUSED, void *devpipe,
            struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    if (*(int *)devpipe >= 0) {
        if (vlp_putpipe(*(int *)devpipe, img_data->image_norm, cam->imgs.size_norm) == -1)
            MOTION_LOG(ERR, TYPE_EVENTS, SHOW_ERRNO
                ,_("Failed to put image into video pipe"));
    }
}
#endif /* defined(HAVE_V4L2) && !defined(BSD)  */

const char *imageext(struct ctx_cam *cam)
{
    if (cam->imgs.picture_type == IMAGE_TYPE_PPM)
        return "ppm";

    if (cam->imgs.picture_type == IMAGE_TYPE_WEBP)
        return "webp";

    return "jpg";
}

static void event_image_detect(struct ctx_cam *cam,
        motion_event type ATTRIBUTE_UNUSED,
        struct image_data *img_data, char *dummy1 ATTRIBUTE_UNUSED,
        void *dummy2 ATTRIBUTE_UNUSED, struct timeval *currenttime_tv)
{
    char fullfilename[PATH_MAX];
    char filename[PATH_MAX];
    int  passthrough;

    if (cam->new_img & NEWIMG_ON) {
        const char *imagepath;

        /*
         *  conf.imagepath would normally be defined but if someone deleted it by control interface
         *  it is better to revert to the default than fail
         */
        if (cam->conf.picture_filename)
            imagepath = cam->conf.picture_filename;
        else
            imagepath = DEF_IMAGEPATH;

        mystrftime(cam, filename, sizeof(filename), imagepath, currenttime_tv, NULL, 0);
        snprintf(fullfilename, PATH_MAX, "%.*s/%.*s.%s"
            , (int)(PATH_MAX-2-strlen(filename)-strlen(imageext(cam)))
            , cam->conf.target_dir
            , (int)(PATH_MAX-2-strlen(cam->conf.target_dir)-strlen(imageext(cam)))
            , filename, imageext(cam));

        passthrough = util_check_passthrough(cam);
        if ((cam->imgs.size_high > 0) && (!passthrough)) {
            put_picture(cam, fullfilename,img_data->image_high, FTYPE_IMAGE);
        } else {
            put_picture(cam, fullfilename,img_data->image_norm, FTYPE_IMAGE);
        }
        event(cam, EVENT_FILECREATE, NULL, fullfilename, (void *)FTYPE_IMAGE, currenttime_tv);
    }
}

static void event_imagem_detect(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data ATTRIBUTE_UNUSED, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *currenttime_tv)
{
    struct config *conf = &cam->conf;
    char fullfilenamem[PATH_MAX];
    char filename[PATH_MAX];
    char filenamem[PATH_MAX];

    if (conf->picture_output_motion) {
        const char *imagepath;

        /*
         *  conf.picture_filename would normally be defined but if someone deleted it by control interface
         *  it is better to revert to the default than fail
         */
        if (cam->conf.picture_filename)
            imagepath = cam->conf.picture_filename;
        else
            imagepath = DEF_IMAGEPATH;

        mystrftime(cam, filename, sizeof(filename), imagepath, currenttime_tv, NULL, 0);

        /* motion images gets same name as normal images plus an appended 'm' */
        snprintf(filenamem, PATH_MAX, "%.*sm"
            , (int)(PATH_MAX-1-strlen(filename))
            , filename);
        snprintf(fullfilenamem, PATH_MAX, "%.*s/%.*s.%s"
            , (int)(PATH_MAX-2-strlen(filenamem)-strlen(imageext(cam)))
            , cam->conf.target_dir
            , (int)(PATH_MAX-2-strlen(cam->conf.target_dir)-strlen(imageext(cam)))
            , filenamem, imageext(cam));
        put_picture(cam, fullfilenamem, cam->imgs.img_motion.image_norm, FTYPE_IMAGE_MOTION);
        event(cam, EVENT_FILECREATE, NULL, fullfilenamem, (void *)FTYPE_IMAGE, currenttime_tv);
    }
}

static void event_image_snapshot(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *currenttime_tv)
{
    char fullfilename[PATH_MAX];
    char filename[PATH_MAX];
    char filepath[PATH_MAX];
    int offset = 0;
    int len = strlen(cam->conf.snapshot_filename);

    if (len >= 9)
        offset = len - 8;

    if (strcmp(cam->conf.snapshot_filename+offset, "lastsnap")) {
        char linkpath[PATH_MAX];
        const char *snappath;
        /*
         *  conf.snapshot_filename would normally be defined but if someone deleted it by control interface
         *  it is better to revert to the default than fail
         */
        if (cam->conf.snapshot_filename)
            snappath = cam->conf.snapshot_filename;
        else
            snappath = DEF_SNAPPATH;

        mystrftime(cam, filepath, sizeof(filepath), snappath, currenttime_tv, NULL, 0);
        snprintf(filename, PATH_MAX, "%.*s.%s"
            , (int)(PATH_MAX-1-strlen(filepath)-strlen(imageext(cam)))
            , filepath, imageext(cam));
        snprintf(fullfilename, PATH_MAX, "%.*s/%.*s"
            , (int)(PATH_MAX-1-strlen(filename))
            , cam->conf.target_dir
            , (int)(PATH_MAX-1-strlen(cam->conf.target_dir))
            , filename);
        put_picture(cam, fullfilename, img_data->image_norm, FTYPE_IMAGE_SNAPSHOT);
        event(cam, EVENT_FILECREATE, NULL, fullfilename, (void *)FTYPE_IMAGE_SNAPSHOT, currenttime_tv);

        /*
         *  Update symbolic link *after* image has been written so that
         *  the link always points to a valid file.
         */
        snprintf(linkpath, PATH_MAX, "%.*s/lastsnap.%s"
            , (int)(PATH_MAX-strlen("/lastsnap.")-strlen(imageext(cam)))
            , cam->conf.target_dir, imageext(cam));

        remove(linkpath);

        if (symlink(filename, linkpath)) {
            MOTION_LOG(ERR, TYPE_EVENTS, SHOW_ERRNO
                ,_("Could not create symbolic link [%s]"), filename);
            return;
        }
    } else {
        mystrftime(cam, filepath, sizeof(filepath), cam->conf.snapshot_filename, currenttime_tv, NULL, 0);
        snprintf(filename, PATH_MAX, "%.*s.%s"
            , (int)(PATH_MAX-1-strlen(imageext(cam)))
            , filepath, imageext(cam));
        snprintf(fullfilename, PATH_MAX, "%.*s/%.*s"
            , (int)(PATH_MAX-1-strlen(filename))
            , cam->conf.target_dir
            , (int)(PATH_MAX-1-strlen(cam->conf.target_dir))
            , filename);
        remove(fullfilename);
        put_picture(cam, fullfilename, img_data->image_norm, FTYPE_IMAGE_SNAPSHOT);
        event(cam, EVENT_FILECREATE, NULL, fullfilename, (void *)FTYPE_IMAGE_SNAPSHOT, currenttime_tv);
    }

    cam->snapshot = 0;
}

/**
 * event_image_preview
 *      event_image_preview
 *
 * Returns nothing.
 */
static void event_image_preview(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data ATTRIBUTE_UNUSED, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *currenttime_tv)
{
    int use_imagepath;
    const char *imagepath;
    char previewname[PATH_MAX];
    char filename[PATH_MAX];
    struct image_data *saved_current_image;
    int passthrough, retcd;

    if (cam->imgs.preview_image.diffs) {
        saved_current_image = cam->current_image;
        cam->current_image = &cam->imgs.preview_image;

        /* Use filename of movie i.o. jpeg_filename when set to 'preview'. */
        use_imagepath = strcmp(cam->conf.picture_filename, "preview");

        if ((cam->movie_output || (cam->conf.movie_extpipe_use && cam->extpipe)) && !use_imagepath) {

            if (cam->conf.movie_extpipe_use && cam->extpipe) {
                retcd = snprintf(previewname, PATH_MAX,"%s.%s"
                    , cam->extpipefilename, imageext(cam));
                if ((retcd < 0) || (retcd >= PATH_MAX)) {
                    MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO
                        ,_("Error creating preview pipe name %d %s")
                        ,retcd, previewname);
                    return;
                }
            } else {
                /* Replace avi/mpg with jpg/ppm and keep the rest of the filename. */
                /* TODO:  Hope that extensions are always 3 bytes*/
                /* -2 to allow for null terminating byte*/
                retcd = snprintf(filename, strlen(cam->newfilename) - 2
                    ,"%s", cam->newfilename);
                if (retcd < 0) {
                    MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO
                        ,_("Error creating file name base %d %s")
                        ,retcd, filename);
                    return;
                }
                retcd = snprintf(previewname, PATH_MAX
                    ,"%s%s", filename, imageext(cam));
                if ((retcd < 0) || (retcd >= PATH_MAX)) {
                    MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO
                        ,_("Error creating preview name %d %s")
                        , retcd, previewname);
                    return;
                }
            }

            passthrough = util_check_passthrough(cam);
            if ((cam->imgs.size_high > 0) && (!passthrough)) {
                put_picture(cam, previewname, cam->imgs.preview_image.image_high , FTYPE_IMAGE);
            } else {
                put_picture(cam, previewname, cam->imgs.preview_image.image_norm , FTYPE_IMAGE);
            }
            event(cam, EVENT_FILECREATE, NULL, previewname, (void *)FTYPE_IMAGE, currenttime_tv);
        } else {
            /*
             * Save best preview-shot also when no movies are recorded or imagepath
             * is used. Filename has to be generated - nothing available to reuse!
             */

            /*
             * conf.picture_filename would normally be defined but if someone deleted it by
             * control interface it is better to revert to the default than fail.
             */
            if (cam->conf.picture_filename)
                imagepath = cam->conf.picture_filename;
            else
                imagepath = (char *)DEF_IMAGEPATH;

            mystrftime(cam, filename, sizeof(filename), imagepath, &cam->imgs.preview_image.timestamp_tv, NULL, 0);
            snprintf(previewname, PATH_MAX, "%.*s/%.*s.%s"
                , (int)(PATH_MAX-2-strlen(filename)-strlen(imageext(cam)))
                , cam->conf.target_dir
                , (int)(PATH_MAX-2-strlen(cam->conf.target_dir)-strlen(imageext(cam)))
                , filename, imageext(cam));

            passthrough = util_check_passthrough(cam);
            if ((cam->imgs.size_high > 0) && (!passthrough)) {
                put_picture(cam, previewname, cam->imgs.preview_image.image_high , FTYPE_IMAGE);
            } else {
                put_picture(cam, previewname, cam->imgs.preview_image.image_norm, FTYPE_IMAGE);
            }
            event(cam, EVENT_FILECREATE, NULL, previewname, (void *)FTYPE_IMAGE, currenttime_tv);
        }

        /* Restore global context values. */
        cam->current_image = saved_current_image;
    }
}


static void event_camera_lost(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data ATTRIBUTE_UNUSED, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    if (cam->conf.on_camera_lost)
        exec_command(cam, cam->conf.on_camera_lost, NULL, 0);
}

static void event_camera_found(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data ATTRIBUTE_UNUSED, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    if (cam->conf.on_camera_found)
        exec_command(cam, cam->conf.on_camera_found, NULL, 0);
}

static void on_movie_end_command(struct ctx_cam *cam,
                                 motion_event type ATTRIBUTE_UNUSED,
                                 struct image_data *dummy ATTRIBUTE_UNUSED, char *filename,
                                 void *arg, struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    int filetype = (unsigned long) arg;

    if ((filetype & FTYPE_MPEG_ANY) && cam->conf.on_movie_end)
        exec_command(cam, cam->conf.on_movie_end, filename, filetype);
}

static void event_extpipe_end(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy ATTRIBUTE_UNUSED, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *currenttime_tv)
{
    if (cam->extpipe_open) {
        cam->extpipe_open = 0;
        fflush(cam->extpipe);
        MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO
            ,_("CLOSING: extpipe file desc %d, error state %d")
            ,fileno(cam->extpipe), ferror(cam->extpipe));
        MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO, "pclose return: %d",
                   pclose(cam->extpipe));
        event(cam, EVENT_FILECLOSE, NULL, cam->extpipefilename, (void *)FTYPE_MPEG, currenttime_tv);
    }
}

static void event_create_extpipe(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy ATTRIBUTE_UNUSED, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *currenttime_tv)
{
    int retcd;

    if ((cam->conf.movie_extpipe_use) && (cam->conf.movie_extpipe)) {
        char stamp[PATH_MAX] = "";
        const char *moviepath;

        /*
         *  conf.mpegpath would normally be defined but if someone deleted it by control interface
         *  it is better to revert to the default than fail
         */
        if (cam->conf.movie_filename) {
            moviepath = cam->conf.movie_filename;
        } else {
            moviepath = DEF_MOVIEPATH;
            MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO, _("moviepath: %s"), moviepath);
        }

        mystrftime(cam, stamp, sizeof(stamp), moviepath, currenttime_tv, NULL, 0);
        snprintf(cam->extpipefilename, PATH_MAX - 4, "%.*s/%.*s"
            , (int)(PATH_MAX-5-strlen(stamp))
            , cam->conf.target_dir
            , (int)(PATH_MAX-5-strlen(cam->conf.target_dir))
            , stamp);

        if (access(cam->conf.target_dir, W_OK)!= 0) {
            /* Permission denied */
            if (errno ==  EACCES) {
                MOTION_LOG(ERR, TYPE_EVENTS, SHOW_ERRNO
                    ,_("no write access to target directory %s"), cam->conf.target_dir);
                return ;
            /* Path not found - create it */
            } else if (errno ==  ENOENT) {
                MOTION_LOG(ERR, TYPE_EVENTS, SHOW_ERRNO
                    ,_("path not found, trying to create it %s ..."), cam->conf.target_dir);
                if (create_path(cam->extpipefilename) == -1)
                    return ;
            }
            else {
                MOTION_LOG(ERR, TYPE_EVENTS, SHOW_ERRNO
                    ,_("error accesing path %s"), cam->conf.target_dir);
                return ;
            }
        }

        /* Always create any path specified as file name */
        if (create_path(cam->extpipefilename) == -1)
            return ;

        mystrftime(cam, stamp, sizeof(stamp), cam->conf.movie_extpipe, currenttime_tv, cam->extpipefilename, 0);

        retcd = snprintf(cam->extpipecmdline, PATH_MAX, "%s", stamp);
        if ((retcd < 0 ) || (retcd >= PATH_MAX)){
            MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO
                , _("Error specifying command line: %s"), cam->extpipecmdline);
            return;
        }
        MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO, _("pipe: %s"), cam->extpipecmdline);

        MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO, "cam->moviefps: %d", cam->movie_fps);

        event(cam, EVENT_FILECREATE, NULL, cam->extpipefilename, (void *)FTYPE_MPEG, currenttime_tv);
        cam->extpipe = popen(cam->extpipecmdline, "we");

        if (cam->extpipe == NULL) {
            MOTION_LOG(ERR, TYPE_EVENTS, SHOW_ERRNO, _("popen failed"));
            return;
        }

        setbuf(cam->extpipe, NULL);
        cam->extpipe_open = 1;
    }
}

static void event_extpipe_put(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    int passthrough;

    /* Check use_extpipe enabled and ext_pipe not NULL */
    if ((cam->conf.movie_extpipe_use) && (cam->extpipe != NULL)) {
        MOTION_LOG(DBG, TYPE_EVENTS, NO_ERRNO, _("Using extpipe"));
        passthrough = util_check_passthrough(cam);
        /* Check that is open */
        if ((cam->extpipe_open) && (fileno(cam->extpipe) > 0)) {
            if ((cam->imgs.size_high > 0) && (!passthrough)){
                if (!fwrite(img_data->image_high, cam->imgs.size_high, 1, cam->extpipe))
                    MOTION_LOG(ERR, TYPE_EVENTS, SHOW_ERRNO
                        ,_("Error writing in pipe , state error %d"), ferror(cam->extpipe));
            } else {
                if (!fwrite(img_data->image_norm, cam->imgs.size_norm, 1, cam->extpipe))
                    MOTION_LOG(ERR, TYPE_EVENTS, SHOW_ERRNO
                        ,_("Error writing in pipe , state error %d"), ferror(cam->extpipe));
           }
        } else {
            MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO
                ,_("pipe %s not created or closed already "), cam->extpipecmdline);
        }
    }
}


static void event_new_video(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy ATTRIBUTE_UNUSED, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *tv1 ATTRIBUTE_UNUSED)
{
    cam->movie_last_shot = -1;

    cam->movie_fps = cam->lastrate;

    MOTION_LOG(INF, TYPE_EVENTS, NO_ERRNO, _("Source FPS %d"), cam->movie_fps);

    if (cam->movie_fps < 2) cam->movie_fps = 2;

}


static void event_movie_newfile(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy0 ATTRIBUTE_UNUSED,
            char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED,
            struct timeval *currenttime_tv)
{
    char stamp[PATH_MAX];
    const char *moviepath;
    const char *codec;
    long codenbr;
    int retcd;

    if (!cam->conf.movie_output && !cam->conf.movie_output_motion)
        return;

    /*
     *  conf.mpegpath would normally be defined but if someone deleted it by control interface
     *  it is better to revert to the default than fail
     */
    if (cam->conf.movie_filename)
        moviepath = cam->conf.movie_filename;
    else
        moviepath = DEF_MOVIEPATH;

    mystrftime(cam, stamp, sizeof(stamp), moviepath, currenttime_tv, NULL, 0);

    /*
     *  motion movies get the same name as normal movies plus an appended 'm'
     *  PATH_MAX - 4 to allow for .mpg to be appended without overflow
     */

     /* The following section allows for testing of all the various containers
      * that Motion permits. The container type is pre-pended to the name of the
      * file so that we can determine which container type created what movie.
      * The intent for this is be used for developer testing when the ffmpeg libs
      * change or the code inside our movie module changes.  For each event, the
      * container type will change.  This way, you can turn on emulate motion, then
      * specify a maximum movie time and let Motion run for days creating all the
      * different types of movies checking for crashes, warnings, etc.
     */
    codec = cam->conf.movie_codec;
    if (strcmp(codec, "ogg") == 0) {
        MOTION_LOG(WRN, TYPE_ENCODER, NO_ERRNO, "The ogg container is no longer supported.  Changing to mpeg4");
        codec = "mpeg4";
    }
    if (strcmp(codec, "test") == 0) {
        MOTION_LOG(NTC, TYPE_ENCODER, NO_ERRNO, "Running test of the various output formats.");
        codenbr = cam->event_nr % 10;
        switch (codenbr) {
        case 1:
            codec = "mpeg4";
            break;
        case 2:
            codec = "msmpeg4";
            break;
        case 3:
            codec = "swf";
            break;
        case 4:
            codec = "flv";
            break;
        case 5:
            codec = "ffv1";
            break;
        case 6:
            codec = "mov";
            break;
        case 7:
            codec = "mp4";
            break;
        case 8:
            codec = "mkv";
            break;
        case 9:
            codec = "hevc";
            break;
        default:
            codec = "msmpeg4";
            break;
        }
        snprintf(cam->motionfilename, PATH_MAX - 4, "%.*s/%s_%.*sm"
            , (int)(PATH_MAX-7-strlen(stamp)-strlen(codec))
            , cam->conf.target_dir, codec
            , (int)(PATH_MAX-7-strlen(cam->conf.target_dir)-strlen(codec))
            , stamp);
        snprintf(cam->newfilename, PATH_MAX - 4, "%.*s/%s_%.*s"
            , (int)(PATH_MAX-6-strlen(stamp)-strlen(codec))
            , cam->conf.target_dir, codec
            , (int)(PATH_MAX-6-strlen(cam->conf.target_dir)-strlen(codec))
            , stamp);
    } else {
        snprintf(cam->motionfilename, PATH_MAX - 4, "%.*s/%.*sm"
            , (int)(PATH_MAX-6-strlen(stamp))
            , cam->conf.target_dir
            , (int)(PATH_MAX-6-strlen(cam->conf.target_dir))
            , stamp);
        snprintf(cam->newfilename, PATH_MAX - 4, "%.*s/%.*s"
            , (int)(PATH_MAX-5-strlen(stamp))
            , cam->conf.target_dir
            , (int)(PATH_MAX-5-strlen(cam->conf.target_dir))
            , stamp);
    }
    if (cam->conf.movie_output) {
        cam->movie_output = mymalloc(sizeof(struct ctx_movie));
        if (cam->imgs.size_high > 0){
            cam->movie_output->width  = cam->imgs.width_high;
            cam->movie_output->height = cam->imgs.height_high;
            cam->movie_output->high_resolution = TRUE;
            cam->movie_output->netcam_data = cam->netcam_high;
        } else {
            cam->movie_output->width  = cam->imgs.width;
            cam->movie_output->height = cam->imgs.height;
            cam->movie_output->high_resolution = FALSE;
            cam->movie_output->netcam_data = cam->netcam;
        }
        cam->movie_output->tlapse = TIMELAPSE_NONE;
        cam->movie_output->fps = cam->movie_fps;
        cam->movie_output->bps = cam->conf.movie_bps;
        cam->movie_output->filename = cam->newfilename;
        cam->movie_output->quality = cam->conf.movie_quality;
        cam->movie_output->start_time.tv_sec = currenttime_tv->tv_sec;
        cam->movie_output->start_time.tv_usec = currenttime_tv->tv_usec;
        cam->movie_output->last_pts = -1;
        cam->movie_output->base_pts = 0;
        cam->movie_output->gop_cnt = 0;
        cam->movie_output->codec_name = codec;
        if (strcmp(cam->conf.movie_codec, "test") == 0) {
            cam->movie_output->test_mode = 1;
        } else {
            cam->movie_output->test_mode = 0;
        }
        cam->movie_output->motion_images = 0;
        cam->movie_output->passthrough =util_check_passthrough(cam);


        retcd = movie_open(cam->movie_output);
        if (retcd < 0){
            MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO
                ,_("Error opening context for movie output."));
            free(cam->movie_output);
            cam->movie_output=NULL;
            return;
        }
        event(cam, EVENT_FILECREATE, NULL, cam->newfilename, (void *)FTYPE_MPEG, currenttime_tv);
    }

    if (cam->conf.movie_output_motion) {
        cam->movie_output_motion = mymalloc(sizeof(struct ctx_movie));
        cam->movie_output_motion->width  = cam->imgs.width;
        cam->movie_output_motion->height = cam->imgs.height;
        cam->movie_output_motion->netcam_data = NULL;
        cam->movie_output_motion->tlapse = TIMELAPSE_NONE;
        cam->movie_output_motion->fps = cam->movie_fps;
        cam->movie_output_motion->bps = cam->conf.movie_bps;
        cam->movie_output_motion->filename = cam->motionfilename;
        cam->movie_output_motion->quality = cam->conf.movie_quality;
        cam->movie_output_motion->start_time.tv_sec = currenttime_tv->tv_sec;
        cam->movie_output_motion->start_time.tv_usec = currenttime_tv->tv_usec;
        cam->movie_output_motion->last_pts = -1;
        cam->movie_output_motion->base_pts = 0;
        cam->movie_output_motion->gop_cnt = 0;
        cam->movie_output_motion->codec_name = codec;
        if (strcmp(cam->conf.movie_codec, "test") == 0) {
            cam->movie_output_motion->test_mode = TRUE;
        } else {
            cam->movie_output_motion->test_mode = FALSE;
        }
        cam->movie_output_motion->motion_images = TRUE;
        cam->movie_output_motion->passthrough = FALSE;
        cam->movie_output_motion->high_resolution = FALSE;
        cam->movie_output_motion->netcam_data = NULL;

        retcd = movie_open(cam->movie_output_motion);
        if (retcd < 0){
            MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO
                ,_("ffopen_open error creating (motion) file [%s]"), cam->motionfilename);
            free(cam->movie_output_motion);
            cam->movie_output_motion = NULL;
            return;
        }
    }
}

static void event_movie_timelapse(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED, struct image_data *img_data,
            char *dummy1 ATTRIBUTE_UNUSED, void *dummy2 ATTRIBUTE_UNUSED,
            struct timeval *currenttime_tv)
{
    int retcd;
    int passthrough;

    if (!cam->movie_timelapse) {
        char tmp[PATH_MAX];
        const char *timepath;
        const char *codec_mpg = "mpg";
        const char *codec_mpeg = "mpeg4";

        /*
         *  conf.timelapse_filename would normally be defined but if someone deleted it by control interface
         *  it is better to revert to the default than fail
         */
        if (cam->conf.timelapse_filename)
            timepath = cam->conf.timelapse_filename;
        else
            timepath = DEF_TIMEPATH;

        mystrftime(cam, tmp, sizeof(tmp), timepath, currenttime_tv, NULL, 0);

        /* PATH_MAX - 4 to allow for .mpg to be appended without overflow */
        snprintf(cam->timelapsefilename, PATH_MAX - 4, "%.*s/%.*s"
            , (int)(PATH_MAX-5-strlen(tmp))
            , cam->conf.target_dir
            , (int)(PATH_MAX-5-strlen(cam->conf.target_dir))
            , tmp);
        passthrough = util_check_passthrough(cam);
        cam->movie_timelapse = mymalloc(sizeof(struct ctx_movie));
        if ((cam->imgs.size_high > 0) && (!passthrough)){
            cam->movie_timelapse->width  = cam->imgs.width_high;
            cam->movie_timelapse->height = cam->imgs.height_high;
            cam->movie_timelapse->high_resolution = TRUE;
        } else {
            cam->movie_timelapse->width  = cam->imgs.width;
            cam->movie_timelapse->height = cam->imgs.height;
            cam->movie_timelapse->high_resolution = FALSE;
        }
        cam->movie_timelapse->fps = cam->conf.timelapse_fps;
        cam->movie_timelapse->bps = cam->conf.movie_bps;
        cam->movie_timelapse->filename = cam->timelapsefilename;
        cam->movie_timelapse->quality = cam->conf.movie_quality;
        cam->movie_timelapse->start_time.tv_sec = currenttime_tv->tv_sec;
        cam->movie_timelapse->start_time.tv_usec = currenttime_tv->tv_usec;
        cam->movie_timelapse->last_pts = -1;
        cam->movie_timelapse->base_pts = 0;
        cam->movie_timelapse->test_mode = FALSE;
        cam->movie_timelapse->gop_cnt = 0;
        cam->movie_timelapse->motion_images = FALSE;
        cam->movie_timelapse->passthrough = FALSE;
        cam->movie_timelapse->netcam_data = NULL;

        if ((strcmp(cam->conf.timelapse_codec,"mpg") == 0) ||
            (strcmp(cam->conf.timelapse_codec,"swf") == 0) ){

            if (strcmp(cam->conf.timelapse_codec,"swf") == 0) {
                MOTION_LOG(WRN, TYPE_EVENTS, NO_ERRNO
                    ,_("The swf container for timelapse no longer supported.  Using mpg container."));
            }

            MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO, _("Timelapse using mpg codec."));
            MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO, _("Events will be appended to file"));

            cam->movie_timelapse->tlapse = TIMELAPSE_APPEND;
            cam->movie_timelapse->codec_name = codec_mpg;
            retcd = movie_open(cam->movie_timelapse);
        } else {
            MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO, _("Timelapse using mpeg4 codec."));
            MOTION_LOG(NTC, TYPE_EVENTS, NO_ERRNO, _("Events will be trigger new files"));

            cam->movie_timelapse->tlapse = TIMELAPSE_NEW;
            cam->movie_timelapse->codec_name = codec_mpeg;
            retcd = movie_open(cam->movie_timelapse);
        }

        if (retcd < 0){
            MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO
                ,_("ffopen_open error creating (timelapse) file [%s]"), cam->timelapsefilename);
            free(cam->movie_timelapse);
            cam->movie_timelapse = NULL;
            return;
        }
        event(cam, EVENT_FILECREATE, NULL, cam->timelapsefilename, (void *)FTYPE_MPEG_TIMELAPSE, currenttime_tv);
    }

    if (movie_put_image(cam->movie_timelapse, img_data, currenttime_tv) == -1) {
        MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO, _("Error encoding image"));
    }

}

static void event_movie_put(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *img_data, char *dummy1 ATTRIBUTE_UNUSED,
            void *dummy2 ATTRIBUTE_UNUSED, struct timeval *currenttime_tv)
{
    if (cam->movie_output) {
        if (movie_put_image(cam->movie_output, img_data, currenttime_tv) == -1){
            MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO, _("Error encoding image"));
        }
    }
    if (cam->movie_output_motion) {
        if (movie_put_image(cam->movie_output_motion, &cam->imgs.img_motion, currenttime_tv) == -1) {
            MOTION_LOG(ERR, TYPE_EVENTS, NO_ERRNO, _("Error encoding image"));
        }
    }
}

static void event_movie_closefile(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy1 ATTRIBUTE_UNUSED,
            char *dummy2 ATTRIBUTE_UNUSED, void *dummy3 ATTRIBUTE_UNUSED,
            struct timeval *currenttime_tv)
{

    if (cam->movie_output) {
        movie_close(cam->movie_output);
        free(cam->movie_output);
        cam->movie_output = NULL;
        event(cam, EVENT_FILECLOSE, NULL, cam->newfilename, (void *)FTYPE_MPEG, currenttime_tv);
    }

    if (cam->movie_output_motion) {
        movie_close(cam->movie_output_motion);
        free(cam->movie_output_motion);
        cam->movie_output_motion = NULL;
        event(cam, EVENT_FILECLOSE, NULL, cam->motionfilename, (void *)FTYPE_MPEG_MOTION, currenttime_tv);
    }

}

static void event_movie_timelapseend(struct ctx_cam *cam,
            motion_event type ATTRIBUTE_UNUSED,
            struct image_data *dummy1 ATTRIBUTE_UNUSED,
            char *dummy2 ATTRIBUTE_UNUSED, void *dummy3 ATTRIBUTE_UNUSED,
            struct timeval *currenttime_tv)
{
    if (cam->movie_timelapse) {
        movie_close(cam->movie_timelapse);
        free(cam->movie_timelapse);
        cam->movie_timelapse = NULL;
        event(cam, EVENT_FILECLOSE, NULL, cam->timelapsefilename, (void *)FTYPE_MPEG_TIMELAPSE, currenttime_tv);
    }
}



/*
 * Starting point for all events
 */

struct event_handlers {
    motion_event type;
    event_handler handler;
};

struct event_handlers event_handlers[] = {
    #if defined(HAVE_MYSQL) || defined(HAVE_PGSQL) || defined(HAVE_SQLITE3) || defined(HAVE_MARIADB)
        {
        EVENT_FILECREATE,
        event_sqlnewfile
        },
    #endif
    {
    EVENT_FILECREATE,
    on_picture_save_command
    },
    {
    EVENT_FILECREATE,
    event_newfile
    },
    {
    EVENT_MOTION,
    event_beep
    },
    {
    EVENT_MOTION,
    on_motion_detected_command
    },
    {
    EVENT_AREA_DETECTED,
    on_area_command
    },
    #if defined(HAVE_MYSQL) || defined(HAVE_PGSQL) || defined(HAVE_SQLITE3) || defined(HAVE_MARIADB)
        {
        EVENT_FIRSTMOTION,
        event_sqlfirstmotion
        },
    #endif
    {
    EVENT_FIRSTMOTION,
    on_event_start_command
    },
    {
    EVENT_ENDMOTION,
    on_event_end_command
    },
    {
    EVENT_IMAGE_DETECTED,
    event_image_detect
    },
    {
    EVENT_IMAGEM_DETECTED,
    event_imagem_detect
    },
    {
    EVENT_IMAGE_SNAPSHOT,
    event_image_snapshot
    },
    #if defined(HAVE_V4L2) && !defined(BSD)
        {
        EVENT_IMAGE,
        event_vlp_putpipe
        },
        {
        EVENT_IMAGEM,
        event_vlp_putpipe
        },
    #endif /* defined(HAVE_V4L2) && !defined(BSD) */
    {
    EVENT_IMAGE_PREVIEW,
    event_image_preview
    },
    {
    EVENT_STREAM,
    event_stream_put
    },
    {
    EVENT_FIRSTMOTION,
    event_new_video
    },
    {
    EVENT_FIRSTMOTION,
    event_movie_newfile
    },
    {
    EVENT_IMAGE_DETECTED,
    event_movie_put
    },
    {
    EVENT_MOVIE_PUT,
    event_movie_put
    },
    {
    EVENT_ENDMOTION,
    event_movie_closefile
    },
    {
    EVENT_TIMELAPSE,
    event_movie_timelapse
    },
    {
    EVENT_TIMELAPSEEND,
    event_movie_timelapseend
    },
    #if defined(HAVE_MYSQL) || defined(HAVE_PGSQL) || defined(HAVE_SQLITE3) || defined(HAVE_MARIADB)
        {
        EVENT_FILECLOSE,
        event_sqlfileclose
        },
    #endif
    {
    EVENT_FILECLOSE,
    on_movie_end_command
    },
    {
    EVENT_FIRSTMOTION,
    event_create_extpipe
    },
    {
    EVENT_IMAGE_DETECTED,
    event_extpipe_put
    },
    {
    EVENT_MOVIE_PUT,
    event_extpipe_put
    },
    {
    EVENT_ENDMOTION,
    event_extpipe_end
    },
    {
    EVENT_CAMERA_LOST,
    event_camera_lost
    },
    {
    EVENT_CAMERA_FOUND,
    event_camera_found
    },
    {0, NULL}
};


/**
 * event
 *   defined with the following parameters:
 *      - Type as defined in event.h (EVENT_...)
 *      - The global context struct cam
 *      - img_data - A pointer to a image_data context used for images
 *      - filename - A pointer to typically a string for a file path
 *      - eventdata - A void pointer that can be cast to anything. E.g. FTYPE_...
 *      - tm - A tm struct that carries a full time structure
 * The split between unsigned images and signed filenames was introduced in 3.2.2
 * as a code reading friendly solution to avoid a stream of compiler warnings in gcc 4.0.
 */
void event(struct ctx_cam *cam, motion_event type, struct image_data *img_data,
           char *filename, void *eventdata, struct timeval *tv1)
{
    int i=-1;

    while (event_handlers[++i].handler) {
        if (type == event_handlers[i].type)
            event_handlers[i].handler(cam, type, img_data, filename, eventdata, tv1);
    }
}
