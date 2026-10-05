#include <saga/edit.hpp>
#include <algorithm>
#include <sstream>
namespace saga {
FileDiff unified_diff(std::string_view before,std::string_view after,std::string_view path,const std::function<void()>& service) {
  FileDiff result;if(before==after)return result;
  auto split=[](std::string_view text){std::vector<std::string> lines;size_t begin=0;while(begin<text.size()){auto end=text.find('\n',begin);if(end==std::string_view::npos)end=text.size()-1;lines.emplace_back(text.substr(begin,end-begin+1));begin=end+1;}return lines;};
  auto a=split(before),b=split(after);int n=static_cast<int>(a.size()),m=static_cast<int>(b.size()),limit=n+m,offset=limit+1;
  std::vector<int> v(static_cast<size_t>(2*limit+3),0);std::vector<std::vector<int>> trace;
  struct Row {char kind;std::string text;};std::vector<Row> rows;
  bool found=false;int distance=0;
  for(int d=0;d<=limit;++d) {
    if(service)service();
    // ponytail: bounded Myers trace; replacement diff remains exact for large dissimilar files.
    if((trace.size()+1)*v.size()>4*1024*1024){result.coarse=true;break;}
    trace.push_back(v);
    for(int k=-d;k<=d;k+=2) {
      int x=k==-d || (k!=d && v[offset+k-1]<v[offset+k+1])?v[offset+k+1]:v[offset+k-1]+1,y=x-k;
      while(x<n && y<m && a[static_cast<size_t>(x)]==b[static_cast<size_t>(y)]){++x;++y;}
      v[offset+k]=x;if(x>=n && y>=m){found=true;distance=d;break;}
    }
    if(found)break;
  }
  if(found) {
    int x=n,y=m;
    for(int d=distance;d>=0;--d) {
      auto& previous=trace[static_cast<size_t>(d)];int k=x-y;
      int pk=k==-d || (k!=d && previous[offset+k-1]<previous[offset+k+1])?k+1:k-1;
      int px=previous[offset+pk],py=px-pk;
      while(x>px && y>py){rows.push_back({' ',a[static_cast<size_t>(--x)]});--y;}
      if(d==0)break;
      if(x==px)rows.push_back({'+',b[static_cast<size_t>(--y)]});else rows.push_back({'-',a[static_cast<size_t>(--x)]});
      x=px;y=py;
    }
    std::reverse(rows.begin(),rows.end());
  } else {for(auto& line:a)rows.push_back({'-',line});for(auto& line:b)rows.push_back({'+',line});}
  std::vector<size_t> old_line(rows.size()+1,1),new_line(rows.size()+1,1);
  for(size_t i=0;i<rows.size();++i){old_line[i+1]=old_line[i]+(rows[i].kind!='+');new_line[i+1]=new_line[i]+(rows[i].kind!='-');result.added+=rows[i].kind=='+';result.removed+=rows[i].kind=='-';}
  std::ostringstream out;out<<"--- a/"<<display_text(std::string(path))<<"\n+++ b/"<<display_text(std::string(path))<<'\n';
  for(size_t i=0;i<rows.size();) {
    while(i<rows.size() && rows[i].kind==' ')++i;
    if(i==rows.size())break;
    size_t first=i>3?i-3:0,last=i+1;
    while(last<rows.size()){size_t next=last;while(next<rows.size() && rows[next].kind==' ')++next;if(next-last>6 || next==rows.size())break;last=next+1;}
    last=std::min(rows.size(),last+3);auto old_count=old_line[last]-old_line[first],new_count=new_line[last]-new_line[first];
    out<<"@@ -"<<(old_count?old_line[first]:old_line[first]-1)<<','<<old_count<<" +"<<(new_count?new_line[first]:new_line[first]-1)<<','<<new_count<<" @@\n";
    for(size_t j=first;j<last;++j){out<<rows[j].kind<<rows[j].text;if(rows[j].text.empty() || rows[j].text.back()!='\n')out<<"\n\\ No newline at end of file\n";}
    i=last;
  }
  result.text=out.str();return result;
}
}
