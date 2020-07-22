/testing/guestbin/swan-prep
# confirm that the network is alive
../../pluto/bin/wait-until-alive -I 192.0.1.254 192.0.2.254
# ensure that clear text does not get through
iptables -A INPUT -i eth1 -s 192.0.2.0/24 -j DROP
iptables -I INPUT -m policy --dir in --pol ipsec -j ACCEPT
# confirm clear text does not get through
../../pluto/bin/ping-once.sh --down -I 192.0.1.254 192.0.2.254
# ipsec start
ipsec _stackmanager start
# disable selinux as we are running stuff from /tmp
setenforce 0
mkdir /tmp/nonroot
cp -a /etc/ipsec.* /tmp/nonroot/
chown -R bin:bin /tmp/nonroot
# secrets must be owned by root - we need per-conn secret whack support
ipsec pluto --config /tmp/nonroot/ipsec.conf --secretsfile /etc/ipsec.secrets --logfile /tmp/pluto.log
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-ipv4-psk
echo "initdone"
