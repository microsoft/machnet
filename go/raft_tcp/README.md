# Raft TCP

(TBD)


### Usage
```bash
make all
```

Server:

Leader:

```bash
./server -local_hostname node0  -app_port 8888 --leader  -alsologtostderr
```

Follower:

```bash
./server -local_hostname node1  -app_port 8889  -alsologtostderr
```

Client

```bash
./client --remote_hostname node0 --app_port 8888 -alsologtostderr
```
