/testing/guestbin/swan-prep --fips
rm /etc/ipsec.d/*db
ipsec initnss > /dev/null 2> /dev/null
/usr/bin/pk12util -i /testing/x509/strongswan/strongWest.p12 -d sql:/etc/ipsec.d -w /testing/x509/nss-pw -k /testing/x509/nss-pw
# Tuomo: why doesn't ipsec checknss --settrust work here?
certutil -M -d sql:/etc/ipsec.d -n "strongSwan CA - strongSwan" -t CT,,
#ipsec start
ipsec _stackmanager start
ipsec pluto --config /etc/ipsec.conf --leak-detective
/testing/pluto/bin/wait-until-pluto-started
ipsec auto --add westnet-eastnet-ikev2
ipsec whack --impair suppress-retransmits
echo "initdone"
