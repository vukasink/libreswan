/testing/guestbin/swan-prep
ip route del 192.0.2.0/24
ipsec start
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add west-any
echo initdone
