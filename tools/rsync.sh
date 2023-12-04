# usage: synch.sh <remote machine> <remote directory>


if [ $# -ne 2 ]; then
    echo "usage: synch.sh <remote machine> <remote directory>"
    exit 1
fi

rsync -avz --exclude-from=rsync-exclude.txt ../ $1:$2


# rsync -avz ../ $1:$2
