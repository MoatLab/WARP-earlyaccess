#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU as a black-box SSD (FTL managed by the device)

#FDP real hw device
#./vfio-bind.sh 0000:8b:00.0

# image directory
IMGDIR=/data/inho/images
# Virtual machine disk image
OSIMGF=/mnt/sda1/fast_ae/u20s.qcow2

# Configurable SSD Controller layout parameter4s (must be power of 2)
secsz=512
secs_per_pg=8		#4KiB 
pgs_per_blk=1024 	#1024*4K:4MB (RU : 256MiB)
#blks_per_pl=1075 	#4MB * 8 * 8 =RU 256MiB -> 1075 = (224*1.2*1024)/256  #! WAF noDIFF
blks_per_pl=985 	#4MB * 8 * 8 =RU 256MiB -> 985 = (224*1.1*1024)/256   #! WAF DIFF
#blks_per_pl=493 	#8MB * 8 * 8 =RU 512MiB -> 985 = (224*1.1*1024)/512 
pls_per_lun=1       # still not support multiplanes		
luns_per_ch=8		# 16G                256*8
nchs=8  			# 128G              256*8*8
ssd_size=229376		# in MegaBytes  
#                     #    128GiB ( 156GiB )
#                     #    256GiB ( 307GiB)   26214
#                     #    512GiB ( 614GiB)   524288 
number_of_ru=$((blks_per_pl * pls_per_lun))
# Latency in nanoseconds
#pg_rd_lat=40000     #40us
#pg_wr_lat=200000    #200us
#blk_er_lat=2000000  #2ms
#ch_xfer_lat=0
pg_rd_lat=10000     #10us
#pg_wr_lat=50000    #50us
#blk_er_lat=500000  #400us
pg_wr_lat=2500    # 500us  : 50us   : 5us : 2.5us
blk_er_lat=10000  # 2000us : 200us  : 20us: 10us


ch_xfer_lat=0
# GC Threshold (1-100)
gc_thres_pcent=90
gc_thres_pcent_high=99

# FDP feature
fdpruhs="0;1;2;3;4;5;6;7"
subsys="femu-subsys-0"
#-----------------------------------------------------------------------

#Compose the entire FEMU BBSSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",secsz=${secsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blks_per_pl=${blks_per_pl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS=${FEMU_OPTIONS}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nchs=${nchs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_high=${gc_thres_pcent_high}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp.ruhs=${fdpruhs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",subsys=${subsys}"
echo ${FEMU_OPTIONS}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and placeit as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

sudo x86_64-softmmu/qemu-system-x86_64 \
    -name "FEMU-BBSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 16 \
    -m 8G \
    -device femu-subsys,id=femu-subsys-0,nqn=subsys0,fdp=on,fdp.nruh=8,fdp.nrg=1,fdp.nru=${number_of_ru} \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::18080-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log

#-device vfio-pci,host=8b:00.0 \
#-virtfs local,path=/data/inho,mount_tag=hds03,security_model=passthrough,id=hds03 \
