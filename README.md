## mod_mbtiles

mod_mbtiles is an Apache module to serve tiles directly from an [.mbtiles](https://github.com/mapbox/mbtiles-spec) file using the familiar /tileset/z/x/y.ext path. Use it to serve your vector tiles made with [tilemaker](https://github.com/systemed/tilemaker).

Serving directly from .mbtiles is fast, space-efficient and simple.

### Installation

(Most of the commands below will require `sudo` unless you're root.)

You'll need Apache's module build tool (apxs) and sqlite3 (because .mbtiles are SQLite databases). Install them on Ubuntu/Debian with:

    apt install apache2-dev libsqlite3-dev

Then to build the module and enable it:

    apxs -lsqlite3 -i -a -c mod_mbtiles.c

### Configuration

In your Apache config file, use `MbtilesEnabled true` to enable vector tile serving for that virtual host, and use `MbtilesAdd` to add each .mbtiles file. For example:

    MbtilesEnabled true
    MbtilesAdd vt "/path/to/my/vector_tiles.mbtiles"
    MbtilesAdd dem "/path/to/my/dem.mbtiles"

This tells Apache to serve the first .mbtiles at `/vt/z/x/y.pbf`, and the second at `/dem/z/x/y.png`. Reload Apache (`service apache2 reload`) to pick up the config change and see it working!

### Details

You can use mod_mbtiles to serve both vector (pbf) and raster (png/jpeg/webp) tiles. You don't need to configure this manually - it's automatically sensed from the metadata in your .mbtiles file.

Your vector tiles should be gzip compressed: mod_mbtiles will serve them with a Content-Encoding header.

Note that `MbtilesEnabled` is a per-directory/host setting, but `MbtilesAdd` is a global setting. So if you want to serve different tilesets from different hosts, make sure you use a different name for each.

There is a maximum of 20 tilesets. You can edit MAX_TILESETS in the source to change this.

### Copyright

Richard Fairhurst, 2022. You may do what you want with this code and there is no warranty.

Based on and heavily expanded from [mod_osm](https://github.com/apeyroux/mod_osm) by Jean-Alexandre Peyroux.
