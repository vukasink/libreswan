ipsec whack --trafficstatus
: ==== cut ====
ipsec auto --status
../../pluto/bin/ipsec-look.sh
: ==== tuc ====
hostname | grep east > /dev/null && ipsec stop
grep "^leak" /tmp/pluto.log
../bin/check-for-core.sh
if [ -f /sbin/ausearch ]; then ausearch -r -m avc -ts recent ; fi
: ==== end ====
