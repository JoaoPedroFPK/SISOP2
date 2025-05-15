mkdir -p servidor
mkdir -p cliente1
mkdir -p cliente2

rm -r cliente1/*
rm -r cliente2/*
rm -r servidor/*

cp server/server servidor/
cp client/client cliente1/
cp client/client cliente2/