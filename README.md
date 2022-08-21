# httpServTT
Http server capable to handle some specific requests. 
Requires boost, and libyaml-cpp-dev

Compile
g++ -std=c++17 main.cpp -o httpServ -L /usr/lib/ -pthread -lyaml-cpp -lboost_iostreams

Run:
sudo ./httpServ yamldir 500

Requests examples:

curl -v -F newArh.z=@example.jpg 192.168.0.1:8080     

curl -X GET -H "Content-type: application/json" -H "Accept: application/json"  -d '{"filename:" :"newArh.z"}' "http://192.168.0.1:8080/" > respondTo.jpg
