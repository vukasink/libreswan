/testing/guestbin/swan-prep --x509
ipsec start
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add north-east
echo initdone
ipsec auto --up north-east
sleep 3
