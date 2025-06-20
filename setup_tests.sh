mkdir -p test_servidor1
mkdir -p test_servidor2
mkdir -p test_cliente1
mkdir -p test_cliente2
mkdir -p test_frontend

rm -r test_cliente1/*
rm -r test_cliente2/*
rm -r test_servidor1/*
rm -r test_servidor2/*
rm -r test_frontend/*

cp server/primary_server test_servidor1/
cp server/backup_server test_servidor2/
cp client/client test_cliente1/
cp client/client test_cliente2/
cp frontend/frontend test_frontend/