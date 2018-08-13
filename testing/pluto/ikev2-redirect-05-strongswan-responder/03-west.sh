/testing/guestbin/swan-prep
ip route del 192.0.2.0/24
ipsec start
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-ikev2
echo "initdone"
ipsec auto --up westnet-eastnet-ikev2
ping -n -c 4 -I 192.0.1.254 192.0.2.254
echo done
