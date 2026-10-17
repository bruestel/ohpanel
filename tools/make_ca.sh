#!/bin/sh
# A CA of your own for every OHpanel of a house: trusted once on each device, uploaded to each panel
# (Settings > Security > Use your own CA). Like the CA a panel makes for itself, it may vouch only for
# local names and private addresses, nothing on the internet.
#
#   tools/make_ca.sh [name] [more names or IPv6 prefixes ...]
#   tools/make_ca.sh "My home CA" panel.myhome.example 2001:db8:1:2::/64
#
# Writes ohpanel-ca.crt (to install on your devices) and ohpanel-ca.key (to upload to the panels, then keep
# it somewhere safe: whoever holds it can vouch for any local address) into the current directory.
set -eu
name=${1:-OHpanel home CA}
[ $# -gt 0 ] && shift
[ -e ohpanel-ca.key ] && { echo "ohpanel-ca.key exists already -- not overwritten" >&2; exit 1; }

permitted="DNS:local,DNS:lan,DNS:home,DNS:home.arpa,DNS:internal,DNS:localdomain,DNS:fritz.box"
permitted="$permitted,IP:10.0.0.0/255.0.0.0,IP:172.16.0.0/255.240.0.0,IP:192.168.0.0/255.255.0.0"
permitted="$permitted,IP:169.254.0.0/255.255.0.0,IP:fc00::/fe00::,IP:fe80::/ffc0::"
for extra in "$@"; do
  case $extra in
    */*)  # an IPv6 prefix, a.b.c.d/n or x:y::/n: as address/mask
      addr=${extra%/*} bits=${extra#*/}
      mask=$(python3 -c "import ipaddress,sys; print(ipaddress.ip_network(sys.argv[1], strict=False).netmask)" "$addr/$bits")
      permitted="$permitted,IP:$addr/$mask" ;;
    *) permitted="$permitted,DNS:$extra" ;;
  esac
done

umask 077
openssl ecparam -name prime256v1 -genkey -noout -out ohpanel-ca.key
umask 022
openssl req -x509 -new -utf8 -key ohpanel-ca.key -sha256 -days 9000 -out ohpanel-ca.crt \
  -subj "/CN=$name/O=OHpanel" \
  -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -addext "subjectKeyIdentifier=hash" \
  -addext "nameConstraints=critical,permitted;$(echo "$permitted" | sed 's/,/,permitted;/g')"
openssl x509 -in ohpanel-ca.crt -noout -subject -enddate -fingerprint -sha256
