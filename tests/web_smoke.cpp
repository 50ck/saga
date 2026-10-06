#include <saga/web.hpp>
#include <saga/web/acquisition.hpp>
#include <iostream>
using namespace saga;
// Optional, explicitly invoked live check; never registered with CTest.
int main(int argc,char** argv) {
  try {
    if(argc==3 && std::string(argv[1])=="--read") {
      web::WebAcquisitionEngine engine;auto acquired=engine.acquire({argv[2],{},2048,true});
      std::cout<<acquired.document.metadata.title<<"\n"<<acquired.document.source.final_url<<"\n"<<acquired.rendered.size()<<" rendered bytes\n"<<acquired.document.source.acquisition_path<<"\n"<<acquired.extraction.json().dump()<<"\n";return 0;
    }
    auto engine=make_search_engine(argc==4 && std::string(argv[1])=="--engine" ? argv[2] : "duckduckgo");SearchRequest request;
    request.query=argc==4 ? argv[3] : argc==2 ? argv[1] : "site:cmake.org CXX_STANDARD 23";
    std::cout<<engine->search(request,{}).dump(2)<<'\n';return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
