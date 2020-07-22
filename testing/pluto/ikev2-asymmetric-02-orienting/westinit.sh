/testing/guestbin/swan-prep
# confirm that the network is alive
../../pluto/bin/wait-until-alive -I 192.0.1.254 192.0.2.254
# ensure that clear text does not get through
iptables -A INPUT -i eth1 -s 192.0.2.0/24 -j DROP
iptables -I INPUT -m policy --dir in --pol ipsec -j ACCEPT
# confirm clear text does not get through
../../pluto/bin/ping-once.sh --down -I 192.0.1.254 192.0.2.254
ipsec start
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-ikev2
# should be unoriented
ipsec auto --status | grep westnet-eastnet-ikev2 | grep orient
ip addr add 192.1.2.46/24 dev eth1
ipsec whack --listen
# should be oriented
ipsec auto --status | grep westnet-eastnet-ikev2 | grep orient
echo "initdone"
