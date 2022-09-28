echo "iface eth0 inet dhcp" >> /etc/network/interfaces
ifup eth0
/etc/init.d/S50sshd restart
