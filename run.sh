# by YYT

NUM_SERVERS=5
# actually number for working is one less, server 0 only for initiation
# all server process is on one machine
NUM_CLIENT_MACHINES=1
NUM_CLIENTS_PER_MACHINE=40
ENABLE_ROCE=1
START_PORT=13000

NUM_CLIENTS=`expr $NUM_CLIENT_MACHINES \* $NUM_CLIENTS_PER_MACHINE`


if [ $# -eq 0 ] # for server: ./run.sh
then 
	end_id=`expr $NUM_SERVERS - 1`
	for i in `seq 0 $end_id`; do
		./main $i $START_PORT $ENABLE_ROCE &
		sleep 0.5
	done
	
elif [ $# -eq 2 ] # for client: ./run.sh 0 127.0.0.1
then 
	end_id=`expr $NUM_CLIENTS_PER_MACHINE - 1`
	for i in `seq 0 $end_id`; do
		id=`expr $1 \* $NUM_CLIENTS_PER_MACHINE + $i`
		./main $id $2 $START_PORT $ENABLE_ROCE &
		sleep 0.5
	done
	
else
	echo "use \"./run [server/client]\""
fi

sleep 1000000000