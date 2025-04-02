FROM ubuntu:22.04

RUN apt-get update && \
    apt-get install -y build-essential g++ make cmake vim git libpthread-stubs0-dev net-tools && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . .

RUN make clean && make

CMD ["/bin/bash"]