#!/bin/sh

# Project:  MapCache
# Purpose:  MapCache tests
# Author:   Even Rouault
#
#*****************************************************************************
# Copyright (c) 2017 Regents of the University of Minnesota.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies of this Software or works derived from this Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
#****************************************************************************/

set -e

mkdir /tmp/mc
sudo chmod -R a+rw /tmp/mc

MAPCACHE_CONF=/tmp/mc/mapcache.xml
echo '<?xml version="1.0" encoding="UTF-8"?>' >> $MAPCACHE_CONF
echo '<mapcache>' >> $MAPCACHE_CONF
echo '    <source name="global-tif" type="gdal">' >> $MAPCACHE_CONF
echo '        <data>/tmp/mc/world.tif</data>' >> $MAPCACHE_CONF
echo '    </source>' >> $MAPCACHE_CONF
echo '    <cache name="disk" type="disk">' >> $MAPCACHE_CONF
echo '        <base>/tmp/mc</base>' >> $MAPCACHE_CONF
echo '    </cache>' >> $MAPCACHE_CONF
echo '    <tileset name="global">' >> $MAPCACHE_CONF
echo '        <cache>disk</cache>' >> $MAPCACHE_CONF
echo '        <source>global-tif</source>' >> $MAPCACHE_CONF
echo '        <grid maxzoom="17">GoogleMapsCompatible</grid>' >> $MAPCACHE_CONF
echo '        <format>JPEG</format>' >> $MAPCACHE_CONF
echo '        <metatile>1 1</metatile>' >> $MAPCACHE_CONF
echo '    </tileset>' >> $MAPCACHE_CONF
echo '    <service type="wmts" enabled="true"/>' >> $MAPCACHE_CONF
echo '    <service type="wms" enabled="true"/>' >> $MAPCACHE_CONF
echo '    <log_level>debug</log_level>' >> $MAPCACHE_CONF
echo '</mapcache>' >> $MAPCACHE_CONF

sudo chown -R www-data:www-data /tmp/mc/
sudo chmod -R 777 /tmp/mc/

cp data/world.tif /tmp/mc

sudo su -c "echo 'LoadModule mapcache_module /usr/lib/apache2/modules/mod_mapcache.so' >> /etc/apache2/apache2.conf"
sudo su -c "echo '<IfModule mapcache_module>' >> /etc/apache2/apache2.conf"
sudo su -c "echo '   <Directory /tmp/mc>' >> /etc/apache2/apache2.conf"
sudo su -c "echo '      Require all granted' >> /etc/apache2/apache2.conf"
sudo su -c "echo '   </Directory>' >> /etc/apache2/apache2.conf"
sudo su -c "echo '   MapCacheAlias /mapcache \"/tmp/mc/mapcache.xml\"' >> /etc/apache2/apache2.conf"
sudo su -c "echo '</IfModule>' >> /etc/apache2/apache2.conf"

sudo service apache2 restart
