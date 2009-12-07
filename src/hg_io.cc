#include "hg_io.h"

#include <sstream>
#include <iostream>

#include <boost/lexical_cast.hpp>

#include "tdict.h"
#include "json_parse.h"
#include "hg.h"

using namespace std;

struct HGReader : public JSONParser {
  HGReader(Hypergraph* g) : rp("[X] ||| "), state(-1), hg(*g), nodes_needed(true), edges_needed(true) { nodes = 0; edges = 0; }

  void CreateNode(const string& cat, const vector<int>& in_edges) {
    WordID c = TD::Convert("X") * -1;
    if (!cat.empty()) c = TD::Convert(cat) * -1;
    Hypergraph::Node* node = hg.AddNode(c, "");
    for (int i = 0; i < in_edges.size(); ++i) {
      if (in_edges[i] >= hg.edges_.size()) {
        cerr << "JSONParser: in_edges[" << i << "]=" << in_edges[i]
             << ", but hg only has " << hg.edges_.size() << " edges!\n";
        abort();
      }
      hg.ConnectEdgeToHeadNode(&hg.edges_[in_edges[i]], node);
    }
  }
  void CreateEdge(const TRulePtr& rule, SparseVector<double>* feats, const SmallVector& tail) {
    Hypergraph::Edge* edge = hg.AddEdge(rule, tail);
    feats->swap(edge->feature_values_);
  }

  bool HandleJSONEvent(int type, const JSON_value* value) {
    switch(state) {
    case -1:
      assert(type == JSON_T_OBJECT_BEGIN);
      state = 0;
      break;
    case 0:
      if (type == JSON_T_OBJECT_END) {
        //cerr << "HG created\n";  // TODO, signal some kind of callback
      } else if (type == JSON_T_KEY) {
        string val = value->vu.str.value;
        if (val == "features") { assert(fdict.empty()); state = 1; }
        else if (val == "is_sorted") { state = 3; }
        else if (val == "rules") { assert(rules.empty()); state = 4; }
        else if (val == "node") { state = 8; }
        else if (val == "edges") { state = 13; }
        else { cerr << "Unexpected key: " << val << endl; return false; }
      }
      break;

    // features
    case 1:
      if(type == JSON_T_NULL) { state = 0; break; }
      assert(type == JSON_T_ARRAY_BEGIN);
      state = 2;
      break;
    case 2:
      if(type == JSON_T_ARRAY_END) { state = 0; break; }
      assert(type == JSON_T_STRING);
      fdict.push_back(FD::Convert(value->vu.str.value));
      break;

    // is_sorted
    case 3:
      assert(type == JSON_T_TRUE || type == JSON_T_FALSE);
      is_sorted = (type == JSON_T_TRUE);
      if (!is_sorted) { cerr << "[WARNING] is_sorted flag is ignored\n"; }
      state = 0;
      break;

    // rules
    case 4:
      if(type == JSON_T_NULL) { state = 0; break; }
      assert(type == JSON_T_ARRAY_BEGIN);
      state = 5;
      break;
    case 5:
      if(type == JSON_T_ARRAY_END) { state = 0; break; }
      assert(type == JSON_T_INTEGER);
      state = 6;
      rule_id = value->vu.integer_value;
      break;
    case 6:
      assert(type == JSON_T_STRING);
      rules[rule_id] = TRulePtr(new TRule(value->vu.str.value));
      state = 5;
      break;

    // Nodes
    case 8:
      assert(type == JSON_T_OBJECT_BEGIN);
      ++nodes;
      in_edges.clear();
      cat.clear();
      state = 9; break;
    case 9:
      if (type == JSON_T_OBJECT_END) {
        //cerr << "Creating NODE\n";
        CreateNode(cat, in_edges);
        state = 0; break;
      }
      assert(type == JSON_T_KEY);
      cur_key = value->vu.str.value;
      if (cur_key == "cat") { assert(cat.empty()); state = 10; break; }
      if (cur_key == "in_edges") { assert(in_edges.empty()); state = 11; break; }
      cerr << "Syntax error: unexpected key " << cur_key << " in node specification.\n";
      return false;
    case 10:
      assert(type == JSON_T_STRING || type == JSON_T_NULL);
      cat = value->vu.str.value;
      state = 9; break;
    case 11:
      if (type == JSON_T_NULL) { state = 9; break; }
      assert(type == JSON_T_ARRAY_BEGIN);
      state = 12; break;
    case 12:
      if (type == JSON_T_ARRAY_END) { state = 9; break; }
      assert(type == JSON_T_INTEGER);
      //cerr << "in_edges: " << value->vu.integer_value << endl;
      in_edges.push_back(value->vu.integer_value);
      break;

    //   "edges": [ { "tail": null, "feats" : [0,1.63,1,-0.54], "rule": 12},
    //         { "tail": null, "feats" : [0,0.87,1,0.02], "rule": 17},
    //         { "tail": [0], "feats" : [1,2.3,2,15.3,"ExtraFeature",1.2], "rule": 13}]
    case 13:
      assert(type == JSON_T_ARRAY_BEGIN);
      state = 14;
      break;
    case 14:
      if (type == JSON_T_ARRAY_END) { state = 0; break; }
      assert(type == JSON_T_OBJECT_BEGIN);
      //cerr << "New edge\n";
      ++edges;
      cur_rule.reset(); feats.clear(); tail.clear();
      state = 15; break;
    case 15:
      if (type == JSON_T_OBJECT_END) {
        CreateEdge(cur_rule, &feats, tail);
        state = 14; break;
      }
      assert(type == JSON_T_KEY);
      cur_key = value->vu.str.value;
      //cerr << "edge key " << cur_key << endl;
      if (cur_key == "rule") { assert(!cur_rule); state = 16; break; }
      if (cur_key == "feats") { assert(feats.empty()); state = 17; break; }
      if (cur_key == "tail") { assert(tail.empty()); state = 20; break; }
      cerr << "Unexpected key " << cur_key << " in edge specification\n";
      return false;
    case 16:    // edge.rule
      if (type == JSON_T_INTEGER) {
        int rule_id = value->vu.integer_value;
        if (rules.find(rule_id) == rules.end()) {
          // rules list must come before the edge definitions!
          cerr << "Rule_id " << rule_id << " given but only loaded " << rules.size() << " rules\n";
          return false;
        }
        cur_rule = rules[rule_id];
      } else if (type == JSON_T_STRING) {
        cur_rule.reset(new TRule(value->vu.str.value));
      } else {
        cerr << "Rule must be either a rule id or a rule string" << endl;
        return false;
      }
      // cerr << "Edge: rule=" << cur_rule->AsString() << endl;
      state = 15;
      break;
    case 17:      // edge.feats
      if (type == JSON_T_NULL) { state = 15; break; }
      assert(type == JSON_T_ARRAY_BEGIN);
      state = 18; break;
    case 18:
      if (type == JSON_T_ARRAY_END) { state = 15; break; }
      if (type != JSON_T_INTEGER && type != JSON_T_STRING) {
        cerr << "Unexpected feature id type\n"; return false;
      }
      if (type == JSON_T_INTEGER) {
        fid = value->vu.integer_value;
        assert(fid < fdict.size());
        fid = fdict[fid];
      } else if (JSON_T_STRING) {
        fid = FD::Convert(value->vu.str.value);
      } else { abort(); }
      state = 19;
      break;
    case 19:
      {
        assert(type == JSON_T_INTEGER || type == JSON_T_FLOAT);
        double val = (type == JSON_T_INTEGER ? static_cast<double>(value->vu.integer_value) :
	                                       strtod(value->vu.str.value, NULL));
        feats.set_value(fid, val);
        state = 18;
        break;
      }
    case 20:     // edge.tail
      if (type == JSON_T_NULL) { state = 15; break; }
      assert(type == JSON_T_ARRAY_BEGIN);
      state = 21; break;
    case 21:
      if (type == JSON_T_ARRAY_END) { state = 15; break; }
      assert(type == JSON_T_INTEGER);
      tail.push_back(value->vu.integer_value);
      break;
    }
    return true;
  }
  string rp;
  string cat;
  SmallVector tail;
  vector<int> in_edges;
  TRulePtr cur_rule;
  map<int, TRulePtr> rules;
  vector<int> fdict;
  SparseVector<double> feats;
  int state;
  int fid;
  int nodes;
  int edges;
  string cur_key;
  Hypergraph& hg;
  int rule_id;
  bool nodes_needed;
  bool edges_needed;
  bool is_sorted;
};

bool HypergraphIO::ReadFromJSON(istream* in, Hypergraph* hg) {
  hg->clear();
  HGReader reader(hg);
  return reader.Parse(in);
}

static void WriteRule(const TRule& r, ostream* out) {
  if (!r.lhs_) { (*out) << "[X] ||| "; }
  JSONParser::WriteEscapedString(r.AsString(), out);
}

bool HypergraphIO::WriteToJSON(const Hypergraph& hg, bool remove_rules, ostream* out) {
  map<const TRule*, int> rid;
  ostream& o = *out;
  rid[NULL] = 0;
  o << '{';
  if (!remove_rules) {
    o << "\"rules\":[";
    for (int i = 0; i < hg.edges_.size(); ++i) {
      const TRule* r = hg.edges_[i].rule_.get();
      int &id = rid[r];
      if (!id) {
        id=rid.size() - 1;
        if (id > 1) o << ',';
        o << id << ',';
        WriteRule(*r, &o);
      };
    }
    o << "],";
  }
  const bool use_fdict = FD::NumFeats() < 1000;
  if (use_fdict) {
    o << "\"features\":[";
    for (int i = 1; i < FD::NumFeats(); ++i) {
      o << (i==1 ? "":",") << '"' << FD::Convert(i) << '"';
    }
    o << "],";
  }
  vector<int> edgemap(hg.edges_.size(), -1);  // edges may be in non-topo order
  int edge_count = 0;
  for (int i = 0; i < hg.nodes_.size(); ++i) {
    const Hypergraph::Node& node = hg.nodes_[i];
    if (i > 0) { o << ","; }
    o << "\"edges\":[";
    for (int j = 0; j < node.in_edges_.size(); ++j) {
      const Hypergraph::Edge& edge = hg.edges_[node.in_edges_[j]];
      edgemap[edge.id_] = edge_count;
      ++edge_count;
      o << (j == 0 ? "" : ",") << "{";

      o << "\"tail\":[";
      for (int k = 0; k < edge.tail_nodes_.size(); ++k) {
        o << (k > 0 ? "," : "") << edge.tail_nodes_[k];
      }
      o << "],";

      o << "\"feats\":[";
      bool first = true;
      for (SparseVector<double>::const_iterator it = edge.feature_values_.begin(); it != edge.feature_values_.end(); ++it) {
        if (!it->second) continue;
        if (!first) o << ',';
        if (use_fdict)
          o << (it->first - 1);
        else
          o << '"' << FD::Convert(it->first) << '"';
        o << ',' << it->second;
        first = false;
      }
      o << "]";
      if (!remove_rules) { o << ",\"rule\":" << rid[edge.rule_.get()]; }
      o << "}";
    }
    o << "],";

    o << "\"node\":{\"in_edges\":[";
    for (int j = 0; j < node.in_edges_.size(); ++j) {
      int mapped_edge = edgemap[node.in_edges_[j]];
      assert(mapped_edge >= 0);
      o << (j == 0 ? "" : ",") << mapped_edge;
    }
    o << "]";
    if (node.cat_ < 0) { o << ",\"cat\":\"" << TD::Convert(node.cat_ * -1) << '"'; }
    o << "}";
  }
  o << "}\n";
  return true;
}

bool needs_escape[128];
void InitEscapes() {
  memset(needs_escape, false, 128);
  needs_escape[static_cast<size_t>('\'')] = true;
  needs_escape[static_cast<size_t>('\\')] = true;
}

string HypergraphIO::Escape(const string& s) {
  size_t len = s.size();
  for (int i = 0; i < s.size(); ++i) {
    unsigned char c = s[i];
    if (c < 128 && needs_escape[c]) ++len;
  }
  if (len == s.size()) return s;
  string res(len, ' ');
  size_t o = 0;
  for (int i = 0; i < s.size(); ++i) {
    unsigned char c = s[i];
    if (c < 128 && needs_escape[c])
      res[o++] = '\\';
    res[o++] = c;
  }
  assert(o == len);
  return res;
}

string HypergraphIO::AsPLF(const Hypergraph& hg, bool include_global_parentheses) {
  static bool first = true;
  if (first) { InitEscapes(); first = false; }
  if (hg.nodes_.empty()) return "()";
  ostringstream os;
  if (include_global_parentheses) os << '(';
  static const string EPS="*EPS*";
  for (int i = 0; i < hg.nodes_.size()-1; ++i) {
    os << '(';
    if (hg.nodes_[i].out_edges_.empty()) abort();
    for (int j = 0; j < hg.nodes_[i].out_edges_.size(); ++j) {
      const Hypergraph::Edge& e = hg.edges_[hg.nodes_[i].out_edges_[j]];
      const string output = e.rule_->e_.size() ==2 ? Escape(TD::Convert(e.rule_->e_[1])) : EPS;
      double prob = log(e.edge_prob_);
      if (isinf(prob)) { prob = -9e20; }
      if (isnan(prob)) { prob = 0; }
      os << "('" << output << "'," << prob << "," << e.head_node_ - i << "),";
    }
    os << "),";
  }
  if (include_global_parentheses) os << ')';
  return os.str();
}

namespace PLF {

const string chars = "'\\";
const char& quote = chars[0];
const char& slash = chars[1];

// safe get
inline char get(const std::string& in, int c) {
  if (c < 0 || c >= (int)in.size()) return 0;
  else return in[(size_t)c];
}

// consume whitespace
inline void eatws(const std::string& in, int& c) {
  while (get(in,c) == ' ') { c++; }
}

// from 'foo' return foo
std::string getEscapedString(const std::string& in, int &c)
{
  eatws(in,c);
  if (get(in,c++) != quote) return "ERROR";
  std::string res;
  char cur = 0;
  do {
    cur = get(in,c++);
    if (cur == slash) { res += get(in,c++); }
    else if (cur != quote) { res += cur; }
  } while (get(in,c) != quote && (c < (int)in.size()));
  c++;
  eatws(in,c);
  return res;
}

// basically atof
float getFloat(const std::string& in, int &c)
{
  std::string tmp;
  eatws(in,c);
  while (c < (int)in.size() && get(in,c) != ' ' && get(in,c) != ')' && get(in,c) != ',') {
    tmp += get(in,c++);
  }
  eatws(in,c);
  if (tmp.empty()) {
    cerr << "Syntax error while reading number! col=" << c << endl;
    abort();
  }
  return atof(tmp.c_str());
}

// basically atoi
int getInt(const std::string& in, int &c)
{
  std::string tmp;
  eatws(in,c);
  while (c < (int)in.size() && get(in,c) != ' ' && get(in,c) != ')' && get(in,c) != ',') {
    tmp += get(in,c++);
  }
  eatws(in,c);
  return atoi(tmp.c_str());
}

// maximum number of nodes permitted
#define MAX_NODES 100000000
// parse ('foo', 0.23)
void ReadPLFEdge(const std::string& in, int &c, int cur_node, Hypergraph* hg) {
  if (get(in,c++) != '(') { assert(!"PCN/PLF parse error: expected ( at start of cn alt block\n"); }
  vector<WordID> ewords(2, 0);
  ewords[1] = TD::Convert(getEscapedString(in,c));
  TRulePtr r(new TRule(ewords));
  // cerr << "RULE: " << r->AsString() << endl;
  if (get(in,c++) != ',') { assert(!"PCN/PLF parse error: expected , after string\n"); }
  size_t cnNext = 1;
  std::vector<float> probs;
  probs.push_back(getFloat(in,c));
  while (get(in,c) == ',') {
    c++;
    float val = getFloat(in,c);
    probs.push_back(val);
    // cerr << val << endl;  //REMO
  }
  //if we read more than one prob, this was a lattice, last item was column increment
  if (probs.size()>1) {
    cnNext = static_cast<size_t>(probs.back());
    probs.pop_back();
    if (cnNext < 1) { cerr << cnNext << endl;
             assert(!"PCN/PLF parse error: bad link length at last element of cn alt block\n"); }
  }
  if (get(in,c++) != ')') { assert(!"PCN/PLF parse error: expected ) at end of cn alt block\n"); }
  eatws(in,c);
  Hypergraph::TailNodeVector tail(1, cur_node);
  Hypergraph::Edge* edge = hg->AddEdge(r, tail);
  //cerr << "  <--" << cur_node << endl;
  int head_node = cur_node + cnNext;
  assert(head_node < MAX_NODES);  // prevent malicious PLFs from using all the memory
  if (hg->nodes_.size() < (head_node + 1)) { hg->ResizeNodes(head_node + 1); }
  hg->ConnectEdgeToHeadNode(edge, &hg->nodes_[head_node]);
  for (int i = 0; i < probs.size(); ++i)
    edge->feature_values_.set_value(FD::Convert("Feature_" + boost::lexical_cast<string>(i)), probs[i]);
}

// parse (('foo', 0.23), ('bar', 0.77))
void ReadPLFNode(const std::string& in, int &c, int cur_node, int line, Hypergraph* hg) {
  //cerr << "PLF READING NODE " << cur_node << endl;
  if (hg->nodes_.size() < (cur_node + 1)) { hg->ResizeNodes(cur_node + 1); }
  if (get(in,c++) != '(') { cerr << line << ": Syntax error 1\n"; abort(); }
  eatws(in,c);
  while (1) {
    if (c > (int)in.size()) { break; }
    if (get(in,c) == ')') {
      c++;
      eatws(in,c);
      break;
    }
    if (get(in,c) == ',' && get(in,c+1) == ')') {
      c+=2;
      eatws(in,c);
      break;
    }
    if (get(in,c) == ',') { c++; eatws(in,c); }
    ReadPLFEdge(in, c, cur_node, hg);
  }
}

} // namespace PLF 

void HypergraphIO::ReadFromPLF(const std::string& in, Hypergraph* hg, int line) {
  hg->clear();
  int c = 0;
  int cur_node = 0;
  if (in[c++] != '(') { cerr << line << ": Syntax error!\n"; abort(); }
  while (1) {
    if (c > (int)in.size()) { break; }
    if (PLF::get(in,c) == ')') {
      c++;
      PLF::eatws(in,c);
      break;
    }
    if (PLF::get(in,c) == ',' && PLF::get(in,c+1) == ')') {
      c+=2;
      PLF::eatws(in,c);
      break;
    }
    if (PLF::get(in,c) == ',') { c++; PLF::eatws(in,c); }
    PLF::ReadPLFNode(in, c, cur_node, line, hg);
    ++cur_node;
  }
  assert(cur_node == hg->nodes_.size() - 1);
}

void HypergraphIO::PLFtoLattice(const string& plf, Lattice* pl) {
  Lattice& l = *pl;
  Hypergraph g;
  ReadFromPLF(plf, &g, 0);
  const int num_nodes = g.nodes_.size() - 1;
  l.resize(num_nodes);
  for (int i = 0; i < num_nodes; ++i) {
    vector<LatticeArc>& alts = l[i];
    const Hypergraph::Node& node = g.nodes_[i];
    const int num_alts = node.out_edges_.size();
    alts.resize(num_alts);
    for (int j = 0; j < num_alts; ++j) {
      const Hypergraph::Edge& edge = g.edges_[node.out_edges_[j]];
      alts[j].label = edge.rule_->e_[1];
      alts[j].cost = edge.feature_values_.value(FD::Convert("Feature_0"));
      alts[j].dist2next = edge.head_node_ - node.id_;
    }
  }
}

namespace B64 {

static const char cb64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char cd64[]="|$$$}rstuvwxyz{$$$$$$$>?@ABCDEFGHIJKLMNOPQRSTUVW$$$$$$XYZ[\\]^_`abcdefghijklmnopq";

static void encodeblock(const unsigned char* in, ostream* os, int len) {
  char out[4];
  out[0] = cb64[ in[0] >> 2 ];
  out[1] = cb64[ ((in[0] & 0x03) << 4) | ((in[1] & 0xf0) >> 4) ];
  out[2] = (len > 1 ? cb64[ ((in[1] & 0x0f) << 2) | ((in[2] & 0xc0) >> 6) ] : '=');
  out[3] = (len > 2 ? cb64[ in[2] & 0x3f ] : '=');
  os->write(out, 4);
}

void b64encode(const char* data, const size_t size, ostream* out) {
  size_t cur = 0;
  while(cur < size) {
    int len = min(static_cast<size_t>(3), size - cur);
    encodeblock(reinterpret_cast<const unsigned char*>(&data[cur]), out, len);
    cur += len;
  }
}

static void decodeblock(const unsigned char* in, unsigned char* out) {   
  out[0] = (unsigned char ) (in[0] << 2 | in[1] >> 4);
  out[1] = (unsigned char ) (in[1] << 4 | in[2] >> 2);
  out[2] = (unsigned char ) (((in[2] << 6) & 0xc0) | in[3]);
}

bool b64decode(const unsigned char* data, const size_t insize, char* out, const size_t outsize) {
  size_t cur = 0;
  size_t ocur = 0;
  unsigned char in[4];
  while(cur < insize) {
    assert(ocur < outsize);
    for (int i = 0; i < 4; ++i) {
      unsigned char v = data[cur];
      v = (unsigned char) ((v < 43 || v > 122) ? '\0' : cd64[ v - 43 ]);
      if (!v) {
        cerr << "B64 decode error at offset " << cur << " offending character: " << (int)data[cur] << endl;
        return false;
      }
      v = (unsigned char) ((v == '$') ? '\0' : v - 61);
      if (v) in[i] = v - 1; else in[i] = 0;
      ++cur;
    }
    decodeblock(in, reinterpret_cast<unsigned char*>(&out[ocur]));
    ocur += 3;
  }
  return true;
}
}

