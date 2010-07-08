char const* usage_name="LanguageModel";
char const* usage_short="srilm.gz [-n FeatureName] [-o StateOrder] [-m LimitLoadOrder]";
char const* usage_verbose="-n determines the name of the feature (and its weight).  -o defaults to 3.  -m defaults to effectively infinite, otherwise says what order lm probs to use (up to).  you could use -o > -m but that would be wasteful.  -o < -m means some ngrams are scored longer (whenever a word is inserted by a rule next to a variable) than the state would ordinarily allow.  NOTE: multiple LanguageModel features are allowed, but they will wastefully duplicate state, except in the special case of -o 1 (which uses no state).  subsequent references to the same a.lm.gz. unless they specify -m, will reuse the same SRI LM in memory; this means that the -m used in the first load of a.lm.gz will take effect.";

//TODO: backoff wordclasses for named entity xltns, esp. numbers.  e.g. digits -> @.  idealy rule features would specify replacement lm tokens/classes

//TODO: extra int in state to hold "GAP" token is not needed.  if there are less than (N-1) words, then null terminate the e.g. left words.  however, this would mean treating gapless items differently.  not worth the potential bugs right now.

//TODO: allow features to reorder by heuristic*weight the rules' terminal phrases (or of hyperedges').  if first pass has pruning, then compute over whole ruleset as part of heuristic

//NOTE: if ngram order is bigger than lm state's, then the longest possible ngram scores are still used.  if you really want a lower order, a truncated copy of the LM should be small enough.  otherwise, an option to null out words outside of the order's window would need to be implemented.

//#define UNIGRAM_DEBUG
#ifdef UNIGRAM_DEBUG
# define UNIDBG(x) do { cerr << x; } while(0)
#else
# define UNIDBG(x)
#endif

#include "ff_lm.h"

#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>

#include <boost/shared_ptr.hpp>
#include <boost/lexical_cast.hpp>

#include "tdict.h"
#include "Vocab.h"
#include "Ngram.h"
#include "hg.h"
#include "stringlib.h"

#ifdef HAVE_RANDLM
// http://randlm.sourceforge.net/
#include "RandLM.h"
#endif

using namespace std;

string LanguageModel::usage(bool param,bool verbose) {
  return usage_helper(usage_name,usage_short,usage_verbose,param,verbose);
}


// NgramShare will keep track of all loaded lms and reuse them.
//TODO: ref counting by shared_ptr?  for now, first one to load LM needs to stick around as long as all subsequent users.

#include <boost/shared_ptr.hpp>
using namespace boost;

//WARNING: first person to add a pointer to ngram must keep it around until others are done using it.
struct NgramShare
{
//  typedef shared_ptr<Ngram> NP;
  typedef Ngram *NP;
  map<string,NP> ns;
  bool have(string const& file) const
  {
    return ns.find(file)!=ns.end();
  }
  NP get(string const& file) const
  {
    assert(have(file));
    return ns.find(file)->second;
  }
  void set(string const& file,NP n)
  {
    ns[file]=n;
  }
  void add(string const& file,NP n)
  {
    assert(!have(file));
    set(file,n);
  }
};

//TODO: namespace or static?
NgramShare ngs;

namespace NgramCache {
  struct Cache {
    map<WordID, Cache> tree;
    float prob;
    Cache() : prob() {}
  };
  static Cache cache_;
  void Clear() { cache_.tree.clear(); }
}

struct LMClient {

  LMClient(string hostname) : port(6666) {
    char const* host=hostname.c_str();
    strcpy(request_buffer, "prob ");
    s = const_cast<char*>(strchr(host, ':'));  // TODO fix const_cast
    if (s != NULL) {
      *s = '\0';
      ++s;
      port = atoi(s);
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    hp = gethostbyname(host);
    if (hp == NULL) {
      cerr << "unknown host " << host << endl;
      abort();
    }
    bzero((char *)&server, sizeof(server));
    bcopy(hp->h_addr, (char *)&server.sin_addr, hp->h_length);
    server.sin_family = hp->h_addrtype;
    server.sin_port = htons(port);

    int errors = 0;
    while (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
      cerr << "Error: connect()\n";
      sleep(1);
      errors++;
      if (errors > 3) exit(1);
    }
    cerr << "Connected to LM on " << host << " on port " << port << endl;
  }

  float wordProb(int word, int* context) {
    NgramCache::Cache* cur = &NgramCache::cache_;
    int i = 0;
    while (context[i] > 0) {
      cur = &cur->tree[context[i++]];
    }
    cur = &cur->tree[word];
    if (cur->prob) { return cur->prob; }

    i = 0;
    int pos = TD::AppendString(word, 5, 16000, request_buffer);
    while (context[i] > 0) {
      assert(pos < 15995);
      request_buffer[pos] = ' ';
      ++pos;
      pos = TD::AppendString(context[i], pos, 16000, request_buffer);
      ++i;
    }
    assert(pos < 15999);
    request_buffer[pos] = '\n';
    ++pos;
    request_buffer[pos] = 0;
    write(sock, request_buffer, pos);
    int r = read(sock, res, 6);
    int errors = 0;
    int cnt = 0;
    while (1) {
      if (r < 0) {
        errors++; sleep(1);
        cerr << "Error: read()\n";
        if (errors > 5) exit(1);
      } else if (r==0 || res[cnt] == '\n') { break; }
      else {
        cnt += r;
        if (cnt==6) break;
        read(sock, &res[cnt], 6-cnt);
      }
    }
    cur->prob = *reinterpret_cast<float*>(res);
    return cur->prob;
  }

 private:
  int sock, port;
  char *s;
  struct hostent *hp;
  struct sockaddr_in server;
  char res[8];
  char request_buffer[16000];
};

class LanguageModelImpl {
  void init(int order) {
    //all these used to be const members, but that has no performance implication, and now there's less duplication.
    order_=order;
    state_size_ = OrderToStateSize(order)-1;
    unigram=(order<=1);
    floor_=-100;
    kSTART = TD::Convert("<s>");
    kSTOP = TD::Convert("</s>");
    kUNKNOWN = TD::Convert("<unk>");
    kNONE = -1;
    kSTAR = TD::Convert("<{STAR}>");
  }

 public:
  explicit LanguageModelImpl(int order) : ngram_(*TD::dict_, order)
  {
    init(order);
  }


//TODO: show that unigram special case (0 state) computes what it should.
  LanguageModelImpl(int order, const string& f, int load_order=0) :
    ngram_(*TD::dict_, load_order ? load_order : order)
  {
    init(order);
    File file(f.c_str(), "r", 0);
    assert(file);
    cerr << "Reading " << order_ << "-gram LM from " << f << endl;
    ngram_.read(file, false);
  }

  virtual ~LanguageModelImpl() {
  }

  Ngram *get_lm() // for make_lm_impl ngs sharing only.
  {
    return &ngram_;
  }


  inline int StateSize(const void* state) const {
    return *(static_cast<const char*>(state) + state_size_);
  }

  inline void SetStateSize(int size, void* state) const {
    *(static_cast<char*>(state) + state_size_) = size;
  }

  virtual double WordProb(int word, int* context) {
    return ngram_.wordProb(word, (VocabIndex*)context);
  }

  /// NOT a negative logp, i.e. should be worse prob = more negative.  that's what SRI wordProb returns, fortunately.
  inline double clamp(double logp) const {
    return logp < floor_ ? floor_ : logp;
  }

  inline double LookupProbForBufferContents(int i) {
//    int k = i; cerr << "P("; while(buffer_[k] > 0) { std::cerr << TD::Convert(buffer_[k++]) << " "; }
    double p = WordProb(buffer_[i], &buffer_[i+1]);
    if (p < floor_) p = floor_;
//    cerr << ")=" << p << endl;
    return p;
  }

  string DebugStateToString(const void* state) const {
    int len = StateSize(state);
    const int* astate = reinterpret_cast<const int*>(state);
    string res = "[";
    for (int i = 0; i < len; ++i) {
      res += " ";
      res += TD::Convert(astate[i]);
    }
    res += " ]";
    return res;
  }

  inline double ProbNoRemnant(int i, int len) {
    int edge = len;
    bool flag = true;
    double sum = 0.0;
    while (i >= 0) {
      if (buffer_[i] == kSTAR) {
        edge = i;
        flag = false;
      } else if (buffer_[i] <= 0) {
        edge = i;
        flag = true;
      } else {
        if ((edge-i >= order_) || (flag && !(i == (len-1) && buffer_[i] == kSTART)))
          sum += LookupProbForBufferContents(i);
      }
      --i;
    }
    return sum;
  }

  double EstimateProb(const vector<WordID>& phrase) {
    int len = phrase.size();
    buffer_.resize(len + 1);
    buffer_[len] = kNONE;
    int i = len - 1;
    for (int j = 0; j < len; ++j,--i)
      buffer_[i] = phrase[j];
    return ProbNoRemnant(len - 1, len);
  }

  //TODO: make sure this doesn't get used in FinalTraversal, or if it does, that it causes no harm.

  //TODO: use stateless_cost instead of ProbNoRemnant, check left words only.  for items w/ fewer words than ctx len, how are they represented?  kNONE padded?

  //TODO: make sure that Vocab_None is set to kNONE in srilm (-1), or that SRILM otherwise interprets -1 as a terminator and not a word
  double EstimateProb(const void* state) {
    if (unigram) return 0.;
    int len = StateSize(state);
    //  << "residual len: " << len << endl;
    buffer_.resize(len + 1);
    buffer_[len] = kNONE;
    const int* astate = reinterpret_cast<const WordID*>(state);
    int i = len - 1;
    for (int j = 0; j < len; ++j,--i)
      buffer_[i] = astate[j];
    return ProbNoRemnant(len - 1, len);
  }

  // for <s> (n-1 left words) and (n-1 right words) </s>
  double FinalTraversalCost(const void* state) {
    if (unigram) return 0.;
    int slen = StateSize(state);
    int len = slen + 2;
    // cerr << "residual len: " << len << endl;
    buffer_.resize(len + 1);
    buffer_[len] = kNONE;
    buffer_[len-1] = kSTART;
    const int* astate = reinterpret_cast<const WordID*>(state);
    int i = len - 2;
    for (int j = 0; j < slen; ++j,--i)
      buffer_[i] = astate[j];
    buffer_[i] = kSTOP;
    assert(i == 0);
    return ProbNoRemnant(len - 1, len);
  }

  /// just how SRILM likes it: [rbegin,rend) is a phrase in reverse word order and null terminated so *rend=kNONE.  return unigram score for rend[-1] plus
  /// cost returned is some kind of log prob (who cares, we're just adding)
  double stateless_cost(WordID *rbegin,WordID *rend) {
    UNIDBG("p(");
    double sum=0;
    for (;rend>rbegin;--rend) {
      sum+=clamp(WordProb(rend[-1],rend));
      UNIDBG(" "<<TD::Convert(rend[-1]));
    }
    UNIDBG(")="<<sum<<endl);
    return sum;
  }

  //TODO: this would be a fine rule heuristic (for reordering hyperedges prior to rescoring.  for now you can just use a same-lm-file -o 1 prelm-rescore :(
  double stateless_cost(TRule const& rule) {
    //TODO: make sure this is correct.
    int len = rule.ELength(); // use a gap for each variable
    buffer_.resize(len + 1);
    WordID * const rend=&buffer_[0]+len;
    *rend=kNONE;
    WordID *r=rend;  // append by *--r = x
    const vector<WordID>& e = rule.e();
    //SRILM is reverse order null terminated
    //let's write down each phrase in reverse order and score it (note: we could lay them out consecutively then score them (we allocated enough buffer for that), but we won't actually use the whole buffer that way, since it wastes L1 cache.
    double sum=0.;
    for (unsigned j = 0; j < e.size(); ++j) {
      if (e[j] < 1) { // variable
        sum+=stateless_cost(r,rend);
        r=rend;
      } else { // terminal
          *--r=e[j];
      }
    }
    // last phrase (if any)
    return sum+stateless_cost(r,rend);
  }

  //NOTE: this is where the scoring of words happens (heuristic happens in EstimateProb)
  double LookupWords(const TRule& rule, const vector<const void*>& ant_states, void* vstate) {
    if (unigram)
      return stateless_cost(rule);
    int len = rule.ELength() - rule.Arity();
    for (int i = 0; i < ant_states.size(); ++i)
      len += StateSize(ant_states[i]);
    buffer_.resize(len + 1);
    buffer_[len] = kNONE;
    int i = len - 1;
    const vector<WordID>& e = rule.e();
    for (int j = 0; j < e.size(); ++j) {
      if (e[j] < 1) {
        const int* astate = reinterpret_cast<const int*>(ant_states[-e[j]]);
        int slen = StateSize(astate);
        for (int k = 0; k < slen; ++k)
          buffer_[i--] = astate[k];
      } else {
        buffer_[i--] = e[j];
      }
    }

    double sum = 0.0;
    int* remnant = reinterpret_cast<int*>(vstate);
    int j = 0;
    i = len - 1;
    int edge = len;

    while (i >= 0) {
      if (buffer_[i] == kSTAR) {
        edge = i;
      } else if (edge-i >= order_) {
        sum += LookupProbForBufferContents(i);
      } else if (edge == len && remnant) {
        remnant[j++] = buffer_[i];
      }
      --i;
    }
    if (!remnant) return sum;

    if (edge != len || len >= order_) {
      remnant[j++] = kSTAR;
      if (order_-1 < edge) edge = order_-1;
      for (int i = edge-1; i >= 0; --i)
        remnant[j++] = buffer_[i];
    }

    SetStateSize(j, vstate);
    return sum;
  }

private:
public:

  static int OrderToStateSize(int order) {
    //TODO: should make the order==0 or not cases virtual overrides (performance gain) except then I have a 2x2 set of options against primary ngram owner vs. copy owner - which is easily factored for a performance loss.  templates would be relatively concise and obviously lose no perf.  honestly why am i even talking about performance?  this is probably irrelevant.  profile.
    return order>1 ?
      ((order-1) * 2 + 1) * sizeof(WordID) + 1
      : 0;
  }

 protected:
  Ngram ngram_;
  vector<WordID> buffer_;
  int order_;
  int state_size_;
  double floor_;
 public:
  WordID kSTART;
  WordID kSTOP;
  WordID kUNKNOWN;
  WordID kNONE;
  WordID kSTAR;
  bool unigram;
};

struct ClientLMI : public LanguageModelImpl
{
  ClientLMI(int order,string const& server) : LanguageModelImpl(order), client_(server)
  {}

  virtual double WordProb(int word, int* context) {
    return client_.wordProb(word, context);
  }

protected:
  LMClient client_;
};

struct ReuseLMI : public LanguageModelImpl
{
  ReuseLMI(int order, Ngram *ng) : LanguageModelImpl(order), ng(ng)
  {}
  double WordProb(int word, int* context) {
    return ng->wordProb(word, (VocabIndex*)context);
  }
protected:
  Ngram *ng;
};

LanguageModelImpl *make_lm_impl(int order, string const& f, int load_order)
{
  if (f.find("lm://") == 0) {
    return new ClientLMI(order,f.substr(5));
  } else if (load_order==0 && ngs.have(f)) {
    cerr<<"Reusing already loaded Ngram LM: "<<f<<endl;
    return new ReuseLMI(order,ngs.get(f));
  } else {
    LanguageModelImpl *r=new LanguageModelImpl(order,f,load_order);
    ngs.add(f,r->get_lm());
    return r;
  }
}

bool parse_lmspec(std::string const& in, int &order, string &featurename, string &filename, int &load_order)
{
  vector<string> const& argv=SplitOnWhitespace(in);
  featurename="LanguageModel";
  order=3;
  load_order=0;
#define LMSPEC_NEXTARG if (i==argv.end()) {            \
    cerr << "Missing argument for "<<*last<<". "; goto usage; \
    } else { ++i; }

  for (vector<string>::const_iterator last,i=argv.begin(),e=argv.end();i!=e;++i) {
    string const& s=*i;
    if (s[0]=='-') {
      if (s.size()>2) goto fail;
      switch (s[1]) {
      case 'o':
        LMSPEC_NEXTARG; order=lexical_cast<int>(*i);
        break;
      case 'n':
        LMSPEC_NEXTARG; featurename=*i;
        break;
      case 'm':
        LMSPEC_NEXTARG; load_order=lexical_cast<int>(*i);
        break;
#undef LMSPEC_NEXTARG
      default:
      fail:
        cerr<<"Unknown LanguageModel option "<<s<<" ; ";
        goto usage;
      }
    } else {
      if (filename.empty())
        filename=s;
      else {
        cerr<<"More than one filename provided. ";
        goto usage;
      }
    }
  }
  if (order > 0 && !filename.empty())
    return true;
usage:
  cerr<<usage_name<<" specification should be: "<<usage_short<<"; you provided: "<<in<<usage_verbose<<endl;
  return false;
}


LanguageModel::LanguageModel(const string& param) {
  int order,load_order;
  string featurename,filename;
  if (!parse_lmspec(param,order,featurename,filename,load_order))
    abort();
  cerr<<"LM feature name: "<<featurename<<" from file "<<filename<<" order "<<order;
  if (load_order)
    cerr<<" loading LM as order "<<load_order;
  cerr<<endl;
  fid_=FD::Convert(featurename);
  pimpl_ = make_lm_impl(order,filename,load_order);
  //TODO: see if it's actually possible to set order_ later to mutate an already used FF for e.g. multipass.  comment in ff.h says only to change state size in constructor.  clone instead?  differently -n named ones from same lm filename are already possible, so no urgency.
  SetStateSize(LanguageModelImpl::OrderToStateSize(order));
}

LanguageModel::~LanguageModel() {
  delete pimpl_;
}

string LanguageModel::DebugStateToString(const void* state) const{
  return pimpl_->DebugStateToString(state);
}

void LanguageModel::TraversalFeaturesImpl(const SentenceMetadata& smeta,
                                          const Hypergraph::Edge& edge,
                                          const vector<const void*>& ant_states,
                                          SparseVector<double>* features,
                                          SparseVector<double>* estimated_features,
                                          void* state) const {
  (void) smeta;
  features->set_value(fid_, pimpl_->LookupWords(*edge.rule_, ant_states, state));
  estimated_features->set_value(fid_, pimpl_->EstimateProb(state));
}

void LanguageModel::FinalTraversalFeatures(const void* ant_state,
                                           SparseVector<double>* features) const {
  features->set_value(fid_, pimpl_->FinalTraversalCost(ant_state));
}

#ifdef HAVE_RANDLM
struct RandLMImpl : public LanguageModelImpl {
  RandLMImpl(int order, randlm::RandLM* rlm) :
      LanguageModelImpl(order),
      rlm_(rlm),
      oov_(rlm->getWordID(rlm->getOOV())),
      rb_(1000, oov_) {
    map<int, randlm::WordID> map_cdec2randlm;
    int max_wordid = 0;
    for(map<randlm::Word, randlm::WordID>::const_iterator it = rlm->vocabStart();
        it != rlm->vocabEnd(); ++it) {
      const int cur = TD::Convert(it->first);
      map_cdec2randlm[TD::Convert(it->first)] = it->second;
      if (cur > max_wordid) max_wordid = cur;
    }
    cdec2randlm_.resize(max_wordid + 1, oov_);
    for (map<int, randlm::WordID>::iterator it = map_cdec2randlm.begin();
         it != map_cdec2randlm.end(); ++it)
      cdec2randlm_[it->first] = it->second;
    map_cdec2randlm.clear();
  }

  inline randlm::WordID Convert2RandLM(int w) {
    return (w < cdec2randlm_.size() ? cdec2randlm_[w] : oov_);
  }

  virtual double WordProb(int word, int* context) {
    int i = order_;
    int c = 1;
    rb_[i] = Convert2RandLM(word);
    while (i > 1 && *context > 0) {
      --i;
      rb_[i] = Convert2RandLM(*context);
      ++context;
      ++c;
    }
    const void* finalState = 0;
    int found;
    //cerr << "I = " << i << endl;
    return rlm_->getProb(&rb_[i], c, &found, &finalState);
  }
 private:
  boost::shared_ptr<randlm::RandLM> rlm_;
  randlm::WordID oov_;
  vector<randlm::WordID> cdec2randlm_;
  vector<randlm::WordID> rb_;
};

LanguageModelRandLM::LanguageModelRandLM(const string& param) :
    fid_(FD::Convert("RandLM")) {
  vector<string> argv;
  int argc = SplitOnWhitespace(param, &argv);
  int order = 3;
  // TODO add support for -n FeatureName
  string filename;
  if (argc < 1) { cerr << "RandLM requires a filename, minimally!\n"; abort(); }
  else if (argc == 1) { filename = argv[0]; }
  else if (argc == 2 || argc > 3) { cerr << "Don't understand 'RandLM " << param << "'\n"; }
  else if (argc == 3) {
    if (argv[0] == "-o") {
      order = atoi(argv[1].c_str());
      filename = argv[2];
    } else if (argv[1] == "-o") {
      order = atoi(argv[2].c_str());
      filename = argv[0];
    }
  }
  set_order(order);
  int cache_MB = 200; // increase cache size
  randlm::RandLM* rlm = randlm::RandLM::initRandLM(filename, order, cache_MB);
  assert(rlm != NULL);
  pimpl_ = new RandLMImpl(order, rlm);
}

LanguageModelRandLM::~LanguageModelRandLM() {
  delete pimpl_;
}

void LanguageModelRandLM::TraversalFeaturesImpl(const SentenceMetadata& smeta,
                                          const Hypergraph::Edge& edge,
                                          const vector<const void*>& ant_states,
                                          SparseVector<double>* features,
                                          SparseVector<double>* estimated_features,
                                          void* state) const {
  (void) smeta;
  features->set_value(fid_, pimpl_->LookupWords(*edge.rule_, ant_states, state));
  estimated_features->set_value(fid_, pimpl_->EstimateProb(state));
}

void LanguageModelRandLM::FinalTraversalFeatures(const void* ant_state,
                                           SparseVector<double>* features) const {
  features->set_value(fid_, pimpl_->FinalTraversalCost(ant_state));
}

#endif

