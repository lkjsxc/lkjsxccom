FROM ubuntu:24.10

RUN apt-get update
RUN apt-get upgrade -y

WORKDIR /data
CMD ["./lkjsxccom"]