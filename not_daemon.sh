sudo chown -R quagga.quagga /etc/quagga
sudo touch /var/run/zebra.pid 
sudo chmod 755 /var/run/zebra.pid 
sudo chown quagga.quagga /var/run/zebra.pid
sudo touch /var/run/ospfd.pid 
sudo chmod 755 /var/run/ospfd.pid 
sudo chown quagga.quagga /var/run/ospfd.pid
sudo touch /var/run/ldpd.pid 
sudo chmod 755 /var/run/ldpd.pid 
sudo chown quagga.quagga /var/run/ldpd.pid
sudo touch /var/run/ldpd.vty 
sudo chmod 755 /var/run/ldpd.vty 
sudo chown quagga.quagga /var/run/ldpd.vty 
sudo chmod 777 /var/run
sudo zebra -d -f /etc/quagga/zebra.conf
sudo ospfd -d -f /etc/quagga/ospfd.conf
sudo ldpd -f /etc/quagga/ldpd.conf
