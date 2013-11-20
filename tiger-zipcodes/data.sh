#!/bin/bash

# Test for dbf_dump.
# which dbf_dump

set -e -u

TMP="$(dirname $0)/tmp"
DEST="$(dirname $0)/layers"

mkdir -p $TMP
mkdir -p $DEST

# Download US CENSUS TIGER 2012 ZCTA shapefile.
if [ ! -f $TMP/tl_2012_us_zcta510.zip ]; then
  curl -sf -o $TMP/tl_2012_us_zcta510.zip http://www2.census.gov/geo/tiger/TIGER2012/ZCTA5/tl_2012_us_zcta510.zip
  unzip -d $TMP $TMP/tl_2012_us_zcta510.zip
fi

# Download US CENSUS 1999 zipcodes for non-ZCTA zips.
if [ ! -f $TMP/zip1999.zip ]; then
  curl -sf -o $TMP/zip1999.zip http://www.census.gov/geo/www/tiger/zip1999.zip
  unzip -d $TMP $TMP/zip1999.zip
  dbf_dump --fields=ZIP_CODE,LATITUDE,LONGITUDE --fs="," $TMP/zipnov99.DBF | tr -d '+ ' > $TMP/zipnov99.csv 
fi

# Move shapefile into postgres for processing.
createdb -U postgres -T template_postgis tmpzip
ogr2ogr -s_srs EPSG:4326 -t_srs EPSG:900913 -f "PostgreSQL" -nlt MULTIPOLYGON -nln data_tmp PG:"host=localhost user=postgres dbname=tmpzip" $TMP/tl_2012_us_zcta510.shp

echo "
CREATE TABLE data AS SELECT ogc_fid, wkb_geometry, zcta5ce10 AS zipcode, CAST(intptlon10 AS float) AS lon, CAST(intptlat10 AS float) AS lat, ST_XMin(ST_Transform(wkb_geometry, 4326))||','||ST_YMin(ST_Transform(wkb_geometry, 4326))||','||ST_XMax(ST_Transform(wkb_geometry, 4326))||','||ST_YMax(ST_Transform(wkb_geometry, 4326)) AS bounds FROM data_tmp;
DROP TABLE data_tmp;
CREATE INDEX data_zip ON data (zipcode);
CREATE INDEX data_geom ON data USING GIST(wkb_geometry);

CREATE TABLE zipnov(zip varchar, lat float, lon float);
COPY zipnov FROM '$(readlink -f $TMP/zipnov99.csv)' DELIMITERS ',' CSV;
CREATE INDEX zipnov_zip ON zipnov (zip);
DELETE FROM zipnov WHERE zip IN (SELECT zip FROM zipnov z LEFT JOIN data d ON z.zip = d.zipcode WHERE d.zipcode IS NOT NULL);
SELECT AddGeometryColumn ('zipnov','geometry',900914,'POINT',2);
CREATE INDEX zipnov_geometry ON zipnov USING GIST(geometry);
UPDATE zipnov SET geometry = st_transform(st_setsrid(st_point(lon,lat),4326),900914);

ALTER TABLE data ADD COLUMN search varchar;
UPDATE data d SET search = d.zipcode||','||(SELECT array_to_string(array_agg(zip),',') FROM zipnov WHERE st_within(geometry, d.wkb_geometry));
UPDATE data d SET search = d.zipcode WHERE d.search IS NULL;
DROP TABLE zipnov;
" | psql -U postgres tmpzip

# Convert to SQLite.
ogr2ogr -f "SQLite" -nln data $DEST/tiger-zipcodes.sqlite PG:"host=localhost user=postgres dbname=tmpzip"

# Cleanup
dropdb -U postgres tmpzip
rm -rf $TMP