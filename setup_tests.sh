mkdir -p test_servidor1
mkdir -p test_servidor2
mkdir -p test_cliente1
mkdir -p test_cliente2
mkdir -p test_frontend

rm -v -r test_cliente1/*
rm -v -r test_cliente2/*
rm -v -r test_servidor1/*
rm -v -r test_servidor2/*
rm -v -r test_frontend/*

cp -v server/primary_server test_servidor1/
cp -v server/backup_server test_servidor2/
cp -v client/client test_cliente1/
cp -v client/client test_cliente2/
cp -v frontend/frontend test_frontend/