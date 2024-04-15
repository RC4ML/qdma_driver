## QDMA Driver Installation

1. prepare clone these repo to your home dir or anywhere you like.
~~~bash
sudo apt-get install libaio1 libaio-dev
git clone https://github.com/RC4ML/qdma_driver.git
cd qdma_driver
~~~

2. compile
~~~bash
make
~~~

3. insmod and config
~~~bash
insmod /path/to/qdma_driver/src/qdma-pf.ko

echo 1024 > /sys/bus/pci/devices/0000:1a:00.0/qdma/qmax
dma-ctl qdma1a000 q add idx 0 mode st dir bi
dma-ctl qdma1a000 q start idx 0 dir bi desc_bypass_en pfetch_bypass_en
~~~
