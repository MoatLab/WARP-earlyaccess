#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU as a black-box SSD (FTL managed by the device)

# image directory
IMGDIR=$HOME/images
# Virtual machine disk image
OSIMGF=$IMGDIR/femu.qcow2
DEVIMG=$IMGDIR/nvme.img
# Configurable SSD Controller layout parameters (must be power of 2)
secsz=512		
secs_per_pg=32		
pgs_per_blk=256 	
blks_per_pl=1024 	
pls_per_lun=1       # still not support multiplanes		
luns_per_ch=8		
nchs=8  			
ssd_size=12288		# in MegaBytes

# Latency in nanoseconds
pg_rd_lat=40000
pg_wr_lat=200000
blk_er_lat=2000000
ch_xfer_lat=0

# GC Threshold (1-100)
gc_thres_pcent=75
gc_thres_pcent_high=95

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
FEMU_OPTIONS=${FEMU_OPTIONS}",subsys=${subsys}"
#FEMU_OPTIONS=${FEMU_OPTIONS}",nruh=${nchs}"

echo ${FEMU_OPTIONS}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi
#    

sudo x86_64-softmmu/qemu-system-x86_64 \
    -name "FEMU-BBSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 4 \
    -m 256G \
    -device femu-subsys,id=femu-subsys-0,nqn=subsys0,fdp=on,fdp.nruh=16,fdp.nrg=1,fdp.nru=128\
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::18080-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log

#-device nvme,id=nvme-ctrl-0,serial=deadbeef,drive=nvm,subsys=nvme-subsys-0\
#-device nvme,id=nvme-ctrl-0,serial=deadbeef
#-drive file=nvm-1.img,if=none,id=nvm-1  

### QEMU nvme fdp opt
# -device nvme-subsys,id=nvme-subsys-0,nqn=subsys0,fdp=on,fdp.nruh=16\
#    -device nvme,serial=deadbeef,subsys=nvme-subsys-0,id=nvme-ctrl-0\
#    -drive file=$DEVIMG,if=none,id=nvm\
#    -device nvme-ns,drive=nvm\
