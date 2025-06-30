mkdir -p servidor
mkdir -p cliente1
mkdir -p cliente2
mkdir -p backup1
mkdir -p backup2

rm -r cliente1/*
rm -r cliente2/*
rm -r servidor/*
rm -r backup1/*
rm -r backup2/*

cp server/server servidor/
cp client/client cliente1/
cp client/client cliente2/
cp server/server backup1/
cp server/server backup2/