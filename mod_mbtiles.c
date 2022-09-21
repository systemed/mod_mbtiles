/*
	Vector tiles .mbtiles server module
	adapted from https://github.com/apeyroux/mod_osm by Jean-Alexandre Peyroux
	see also https://github.com/kd2org/apache-sqliteblob

	To install:
		sudo apxs -lsqlite3 -i -a -c mod_mbtiles.c && sudo service apache2 restart

	To configure Apache:
		MbtilesEnabled true
        MbtilesAdd vt "/path/to/my/vector_tiles.mbtiles"
        MbtilesAdd dem "/path/to/my/dem.mbtiles"

	Note that MbtilesEnabled applies per-directory, while MbtilesAdd is global (across all virtual hosts)
*/

#include "httpd.h" 
#include "http_config.h" 
#include "http_core.h" 
#include "http_log.h" 
#include "http_main.h" 
#include "http_protocol.h" 
#include "http_request.h" 
#include "util_script.h" 
#include "http_connection.h"

#include <sqlite3.h>

#define MAX_TILESETS 20

typedef struct {
	int opened;
	char path[255];
	char name[40];
	char format[40];
	int isPBF; 
	sqlite3 *db;
} Tileset;

typedef struct {
	char context[256];
	int enabled;
} DirectoryConfig;

static void mbtiles_register_hooks (apr_pool_t *p);
int findTileset(const char* name);
static int mbtiles_handler(request_rec *r);
void *mbtiles_create_dir_conf(apr_pool_t *pool, char *context);
const char *mbtiles_add_path(cmd_parms *cmd, void *cfg, const char *name, const char *path);
const char *mbtiles_set_enabled(cmd_parms *cmd, void *cfg, const char *arg);
static Tileset tilesets[MAX_TILESETS];
static int numLoaded = 0;
static DirectoryConfig config;

static unsigned char EMPTY_TILE[36] = { 0x1F,0x8B,0x08,0x00,0xFA,0x78,0x18,0x5E,0x00,0x03,0x93,0xE2,0xE3,0x62,0x8F,0x8F,0x4F,0xCD,0x2D,0x28,0xA9,
	0xD4,0x68,0x50,0xA8,0x60,0x02,0x00,0x64,0x71,0x44,0x36,0x10,0x00,0x00,0x00 };

static const command_rec mbtiles_directives[] = {
	AP_INIT_TAKE1("MbtilesEnabled", mbtiles_set_enabled, NULL, OR_ALL, "Enable or disable mod_mbtiles"),
	AP_INIT_TAKE2("MbtilesAdd", mbtiles_add_path, NULL, OR_ALL, "The tileset name and path to an .mbtiles file."),
	{ NULL }
};

module AP_MODULE_DECLARE_DATA mbtiles_module = {
	STANDARD20_MODULE_STUFF,
	mbtiles_create_dir_conf,/* Per-directory configuration handler */
	NULL,	/* Merge handler for per-directory configurations */
	NULL,	/* Per-server configuration handler */
	NULL,	/* Merge handler for per-server configurations */
	mbtiles_directives,		/* Any directives we may have for httpd */
	mbtiles_register_hooks 	/* Our hook registering function */
};

void *mbtiles_create_dir_conf(apr_pool_t *pool, char *context) {
	context = context ? context : "(undefined context)";
	DirectoryConfig *cfg = apr_pcalloc(pool, sizeof(DirectoryConfig));
	if (cfg) {
		strcpy(cfg->context, context);
		cfg->enabled = 0;
	}
	return cfg;
}

const char *mbtiles_set_enabled(cmd_parms *cmd, void *cfg, const char *arg) {
	DirectoryConfig *config = (DirectoryConfig*) cfg; // cast void pointer to DirectoryConfig
	if(!ap_cstr_casecmp(arg, "true")) 
		config->enabled = 1;
	else 
		config->enabled = 0;
	return NULL;
}

const char *mbtiles_add_path(cmd_parms *cmd, void *cfg, const char *name, const char *path) {
	// we ignore config because tilesets are loaded globally
	if (findTileset(name)>-1) return NULL; // don't reload if we already have one
	if (numLoaded==MAX_TILESETS) { 
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, cmd->server, "Maximum tilesets already loaded");
		return NULL;
	}
	Tileset tileset;
	tileset.opened = 0;
	strcpy(tileset.path, path);
	strcpy(tileset.name, name);
	tilesets[numLoaded] = tileset;
	numLoaded++;
	return NULL;
}

static void processStarting(apr_pool_t *pool, server_rec *s) {
	for (int i=0; i<numLoaded; i++) {
		// Attempt to open the database
		if (SQLITE_OK!=sqlite3_open_v2(tilesets[i].path, &tilesets[i].db, SQLITE_OPEN_READONLY, NULL)) {
			sqlite3_close(tilesets[i].db);
			tilesets[i].opened = 0;
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "Couldn't open mbtiles");
			return;
		}

		// Successfully opened, so find out what format it is
		const char *sql = "SELECT value FROM metadata WHERE name='format';";
		sqlite3_stmt *pStmt;
		int rc = sqlite3_prepare(tilesets[i].db, sql, -1, &pStmt, 0);
		if (rc!=SQLITE_OK) { ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "Couldn't find format in mbtiles"); return; }
		rc = sqlite3_step(pStmt);
		if (rc!=SQLITE_ROW) { ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "Couldn't find format in mbtiles"); return; }
		const char *fmt = sqlite3_column_text(pStmt, 0);
		strcpy(tilesets[i].format, fmt);
		rc = sqlite3_finalize(pStmt);

		// All good!
		tilesets[i].opened = 1;
		tilesets[i].isPBF = (strcmp(tilesets[i].format,"pbf")==0) ? 1 : 0;
		ap_log_error(APLOG_MARK, APLOG_INFO, 0, s, tilesets[i].isPBF ? "Successfully opened vector mbtiles" : "Successfully opened raster mbtiles");
	}
}
static apr_status_t processEnding(void *d) {
	for (int i=0; i<numLoaded; i++) {
		sqlite3_close(tilesets[i].db);
		tilesets[i].opened = 0;
	}
}

static void mbtiles_register_hooks(apr_pool_t *p) { 
	ap_hook_handler(mbtiles_handler, NULL, NULL, APR_HOOK_FIRST);
	apr_pool_cleanup_register(p, NULL, processEnding, apr_pool_cleanup_null);
	ap_hook_child_init(processStarting, NULL, NULL, APR_HOOK_FIRST);
}

static int readTile(sqlite3 *db, const int z, const int x, const int y, unsigned char **pTile, int *psTile ) {
	const char *sql = "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?;";
	sqlite3_stmt *pStmt;
	int rc; // sqlite return code
	*pTile = NULL;

	do {
		rc = sqlite3_prepare(db, sql, -1, &pStmt, 0);
		if(rc!=SQLITE_OK) { return rc; }

		sqlite3_bind_int(pStmt, 1, z);
		sqlite3_bind_int(pStmt, 2, x);
		sqlite3_bind_int(pStmt, 3, y);
		
		rc = sqlite3_step(pStmt);
		if( rc==SQLITE_ROW ){
			*psTile = sqlite3_column_bytes(pStmt, 0);
			*pTile = (unsigned char *)malloc(*psTile);
			memcpy(*pTile, sqlite3_column_blob(pStmt, 0), *psTile);
		}

		rc = sqlite3_finalize(pStmt);

	} while(rc==SQLITE_SCHEMA);

	return rc;
}

int findTileset(const char* name) {
	for (int i=0; i<numLoaded; i++) {
		if (strcmp(tilesets[i].name, name)==0) { return i; }
	}
	return -1;
}

static int mbtiles_handler(request_rec *r) {
	DirectoryConfig *config = (DirectoryConfig*) ap_get_module_config(r->per_dir_config, &mbtiles_module);
	if (config->enabled == 0) return(DECLINED);

	unsigned char *tile;
	int tileSize=0, z=-1, x=0, y=0;
	char name[40];
	tile = NULL;

	// pattern-matching: could replace this with a regex (http://svn.apache.org/repos/asf/httpd/sandbox/replacelimit/include/ap_regex.h)
	sscanf(r->uri, "/%39[^/]/%d/%d/%d.", name, &z, &x, &y);
	if (z==-1) { return(DECLINED); } // pattern didn't match

	// find which tileset it is
	int c = findTileset(name);
	if (c==-1) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "couldn't find tileset");
		return(DECLINED);
	}
	
	// we now have a tileset, so test it's open
	if (tilesets[c].opened == 0) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "mbtiles file isn't open");
		return HTTP_INTERNAL_SERVER_ERROR;
	}

	// invert y for TMS
	y = ((1 << z) - y - 1);

	// read tile
	if (SQLITE_OK!=readTile(tilesets[c].db, z, x, y, &tile, &tileSize) ) {
		// SQLite error
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "sqlite error while reading %d/%d/%d from mbtiles", z, x, y);
		sqlite3_close(tilesets[c].db);
		return(DECLINED);
	} else if (NULL == tile && tilesets[c].isPBF) {
		// Vector tile not found
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Tile %d/%d/%d not found", z, x, y);
		ap_set_content_type(r, "application/x-protobuf");  
		apr_table_setn(r->headers_out, "Content-Encoding", "gzip");
		ap_set_content_length(r, 36);
		ap_rwrite(EMPTY_TILE, 36, r);
	} else if (NULL == tile) {
		// Raster tile not found
		return(HTTP_NOT_FOUND);
	} else if (tilesets[c].isPBF) {
		// Write vector tile
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Writing vector tile (size:%d) : %d/%d/%d", tileSize, z, x, y);
		ap_set_content_type(r, "application/x-protobuf");  
		apr_table_setn(r->headers_out, "Content-Encoding", "gzip");
		ap_set_content_length(r, tileSize);
		ap_rwrite(tile, tileSize, r);
	} else {
		// Write raster tile
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Writing raster tile (size:%d) : %d/%d/%d", tileSize, z, x, y);
		if      (strcmp(tilesets[c].format,"png" )==0) { ap_set_content_type(r, "image/png"); }
		else if (strcmp(tilesets[c].format,"jpg" )==0) { ap_set_content_type(r, "image/jpeg"); }
		else if (strcmp(tilesets[c].format,"webp")==0) { ap_set_content_type(r, "image/webp"); }
		else { ap_set_content_type(r,tilesets[c].format); }
		ap_set_content_length(r, tileSize);
		ap_rwrite(tile, tileSize, r);
	}

	free(tile);
	return OK; 
}