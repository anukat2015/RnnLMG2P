#include <fst/fstlib.h>
#include "rnnlmlib.h"
#include "BRnnHash.h"
///// fast exp() implementation
static union{
  double d;
  struct{
    int j,i;
  } n;
} d2i;
#define EXP_A (1048576/M_LN2)
#define EXP_C 60801
#define FAST_EXP(y)(d2i.n.i=EXP_A*(y)+(1072693248-EXP_C),d2i.d)


class Token {
 public:
  Token (int hsize, int max_order) 
    : word (0), weight (0.0), total (0.0), 
      prev (NULL), state (0), key (-1) {
    hlayer.resize (hsize, 1.0);
    history.resize (max_order, 0);

    HashHistory ();
  }

  Token (Token* tok, int w, int s) 
    : word (w), weight (0.0), total (0.0), 
      prev (tok), state (s), key (-1) {
    //Copy an existing token and update the 
    // various layers as needed
    hlayer.resize (tok->hlayer.size(), 0.0);
    history.resize (tok->history.size (), 0);
    
    for (int i = tok->history.size () - 1; i > 0; i--)
      history [i] = tok->history [i - 1];
    history [0] = tok->word;

    HashHistory ();
  }

  void HashHistory () {
    for (auto& i : history) 
      hhash = hhash * 7877 + i;
  }

  int state;
  mutable int word;
  mutable int key;
  mutable double weight;
  mutable double total;
  mutable Token* prev;
  vector<double> hlayer;
  vector<int> history;
  size_t hhash;
};

class TokenCompare {
 public:
  bool operator () (const Token& t1, const Token& t2) const {
    return (t1.state == t2.state &&
	    t1.word == t2.word &&
	    t1.hhash == t2.hhash);
  }
};

class TokenHash {
 public:
  size_t operator () (const Token& t) const {
    return t.state * kPrime0 + t.word * kPrime1 + t.hhash * kPrime2;
  }
 private:
  static const size_t kPrime0;
  static const size_t kPrime1;
  static const size_t kPrime2;
};
const size_t TokenHash::kPrime0 = 7853;
const size_t TokenHash::kPrime1 = 7867;
const size_t TokenHash::kPrime2 = 7873;

typedef unordered_set<Token, TokenHash, TokenCompare> TokenSet;

class TokenIteratorCompare {
 public:
  bool operator () (const TokenSet::iterator t1, const TokenSet::iterator t2) {
    return (t1->total < t2->total);
  }
};


class SharedRnnLM {
 public:
  SharedRnnLM (BRnnHash& hash) : h (hash) { }

  double ComputeNet (const Token& p, Token* t) {
    vector<double> olayer;
    olayer.resize (osize, 0.0);
    
    for (int j = 0; j < hsize; j++)
      for (int i = 0; i < hsize; i++)
	t->hlayer [j] += p.hlayer [i] * syn0 [i + h.vocab_.size () + j * isize];

    for (int i = 0; i < hsize; i++)
      if (p.word != -1)
	t->hlayer [i] += syn0 [p.word + i * (hsize + h.vocab_.size ())];

    for (int i = 0; i < hsize; i++) {
      if (t->hlayer [i] > 50)
	t->hlayer [i] = 50;
      if (t->hlayer [i] < -50)
	t->hlayer [i] = -50;
      t->hlayer [i] = 1 / (1 + FAST_EXP (-t->hlayer [i]));
    }

    for (int j = h.vocab_.size (); j < osize; j++) 
      for (int i = 0; i < hsize; i++)
	olayer [j] += t->hlayer [i] * syn1 [i + j * hsize];

    //Begin class direct connection activations
    if (synd.size () > 0) {
      //Feature hash begin
      vector<unsigned long long> hash;
      hash.resize (max_order, 0);

      for (int i = 0; i < order; i++) {
	if (i > 0)
	  if (t->history [i - 1] == -1)
	    break;
	hash [i] = h.primes_[0] * h.primes_[1];
	for (int j = 1; j <= i; j++)
	  hash [i] += 
	    h.primes_[(i * h.primes_[j] + j) % h.primes_.size ()]
	    * (unsigned long long) (t->history [j - 1] + 1);

	hash [i] = hash [i] % (synd.size () / 2);
      }
      //Feature hash end
      for (int i = h.vocab_.size (); i < osize; i++) {
	for (int j = 0; j < order; j++) {
	  if (hash [j]) {
	    olayer [i] += synd [hash [j]];
	    hash [j]++;
	  } else {
	    break;
	  }
	}
      }
    }
    //End class direct connection activations

    double sum = 0;    
    //Softmax on classes
    for (int i = h.vocab_.size (); i < osize; i++) {
      if (olayer [i] > 50)
	olayer [i] = 50;
      if (olayer [i] < -50)
	olayer [i] = -50;
      double val = FAST_EXP (olayer [i]);
      sum += val;
      olayer [i] = val;
    }
    for (int i = h.vocab_.size (); i < osize; i++) 
      olayer [i] /= sum;

    //1->2 word activations
    if (t->word != -1) {
      int begin = h.class_sizes_[h.vocab_[t->word].class_index].begin;
      int end   = h.class_sizes_[h.vocab_[t->word].class_index].end;
      for (int j = begin; j <= end; j++)
	for (int i = 0; i < hsize; i++)
	  olayer [j] += t->hlayer [i] * syn1 [i + j * hsize];

      //Begin word direct connection activations
      if (synd.size () > 0) {
	//Begin feature hashing
	unsigned long long hash [max_order];
	for (int i = 0; i < order; i++)
	  hash [i] = 0;

	for (int i = 0; i < order; i++) {
	  if (i > 0)
	    if (t->history [i - 1] == -1)
	      break;

	  hash [i] = h.primes_[0] * h.primes_[1]
	    * (unsigned long long) (h.vocab_[t->word].class_index + 1);
	  
	  for (int j = 1; j <= i; j++)
	    hash [i] += h.primes_[(i * h.primes_[j] + j) % h.primes_.size ()]
	      * (unsigned long long) (t->history [j - 1] + 1);
	  
	  hash [i] = (hash [i] % (synd.size () / 2)) + (synd.size () / 2);
	}
	//End feature hashing

	for (int i = begin; i <= end; i++) {
	  for (int j = 0; j < order; j++) {
	    if (hash [j]) {
	      olayer [i] += synd [hash [j]];
	      hash [j]++;
	      hash [j] = hash [j] % synd.size ();
	    } else {
	      break;
	    }
	  }
	}
      }
      //End word direct connection activations

      sum = 0.0;
      for (int i = begin; i <= end; i++) {
	if (olayer [i] > 50)
	  olayer [i] = 50;
	if (olayer [i] < -50)
	  olayer [i] = -50;
	olayer [i] = FAST_EXP (olayer [i]);
	sum += olayer [i];
      }
      for (int i = begin; i <= end; i++)
	olayer [i] /= sum;
    }

    //double prob = - log (olayer [t->word] * olayer [h.vocab_.size () + h.vocab_[t->word].class_index]);
    double prob = olayer [t->word] * olayer [h.vocab_.size () + h.vocab_[t->word].class_index];
    /*
    cout << "Current: " << t->word << ": " << ((t->word == -1) ? "--" : h.vocab_[t->word].word)
	 << " P (w|C): " << olayer [t->word]
	 << " P (C): " << olayer [h.vocab_.size () + h.vocab_[t->word].class_index]
	 << " P (w): " << prob
	 << endl;
    */
    return prob;
  }

  //We need the synapses and the vocabulary hash
  int isize;
  int hsize;
  int osize;
  int order;
  int max_order;
  vector<double> syn0;
  vector<double> syn1;
  vector<double> synd;
  BRnnHash& h;
};

class RnnLMReader {
 public:
  RnnLMReader (string rnnlm_file) {
    srand (1);
    //We don't actually need or use any of this
    rnnlm_.setLambda (0.75);
    rnnlm_.setRegularization (0.0000001);
    rnnlm_.setDynamic (false);
    rnnlm_.setRnnLMFile ((char*)rnnlm_file.c_str ());
    rnnlm_.setRandSeed (1);
    rnnlm_.useLMProb (false);
    rnnlm_.setDebugMode (1);
    //This will actually load the thing
    rnnlm_.restoreNet ();
  }

  SharedRnnLM CopySharedRnnLM (BRnnHash& h) {
    //Copy static data that can be shared by all tokens
    SharedRnnLM m (h);
    for (int i = 0; i < rnnlm_.layer0_size * rnnlm_.layer1_size; i++) 
      m.syn0.push_back ((double)rnnlm_.syn0 [i].weight);

    for (int i = 0; i < rnnlm_.layer1_size * rnnlm_.layer2_size; i++) 
      m.syn1.push_back ((double)rnnlm_.syn1 [i].weight);
    
    for (int i = 0; i < rnnlm_.direct_size; i++)
      m.synd.push_back ((double)rnnlm_.syn_d [i]);

    m.order = rnnlm_.direct_order;
    m.isize = rnnlm_.layer0_size;
    m.hsize = rnnlm_.layer1_size;
    m.osize = rnnlm_.layer2_size;
    m.order = rnnlm_.direct_order;
    m.max_order = 5; //MAX_NGRAM_ORDER;
    return m;
  }

  BRnnHash CopyVocabHash () {
    BRnnHash h (rnnlm_.class_size);
    for (int i = 0; i < rnnlm_.vocab_size; i++) {
      string word = rnnlm_.vocab [i].word;
      h.AddWordToVocab (word, rnnlm_.vocab [i].cn);
    }
    h.SortVocab ();
    h.SetClasses ();
    for (int i = 0; i < h.vocab_.size (); i++)
      h.MapToken (h.vocab_[i].word);

    return h;
  }

  CRnnLM rnnlm_; //The actual model
};

vector<int> SplitSentence (string& line, const BRnnHash& h) {

  std::string word;
  std::vector<int> words;
  std::stringstream ss (line);
  while (ss >> word) {
    words.push_back (h.GetWordId (word));
  }
  words.push_back (0);
  return words;
}

void LoadCorpus (std::string& filename,
                 std::vector<std::vector<int> >* corpus, const BRnnHash& h){
  std::ifstream ifp (filename.c_str ());
  std::string line;

  if (ifp.is_open ()) {
    while (ifp.good ()) {
      getline (ifp, line);
      if (line.empty ())
        continue;
      
      std::string word;
      std::vector<int> words;
      std::stringstream ss (line);
      while (ss >> word)
        words.push_back (h.GetWordId (word));
      
      words.push_back (0);
      corpus->push_back (words);
    }
    ifp.close ();
  }
}

void LoadTestSet (std::string& filename,
		  std::vector<std::vector<std::string> >* corpus) {
  std::ifstream ifp (filename.c_str ());
  std::string line;

  if (ifp.is_open ()) {
    while (ifp.good ()) {
      getline (ifp, line);
      if (line.empty ())
        continue;
      
      std::string word;
      std::vector<string> words;
      std::stringstream ss (line);
      while (ss >> word)
        words.push_back (word);
      
      words.push_back ("</s>");
      corpus->push_back (words);
    }
    ifp.close ();
  }
}

VectorFst<StdArc> WordToFst (vector<string>& word, BRnnHash& h) {
  VectorFst<StdArc> fst;
  fst.AddState ();
  fst.SetStart (0);
  for (int i = 0; i < word.size (); i++) {
    int hash = h.HashInput (word.begin () + i,
			    word.begin () + i + 1);
    fst.AddState ();
    fst.AddArc (i, StdArc (hash, hash, StdArc::Weight::One(), i + 1));
  }

  for (int i = 0; i < word.size (); i++) {
    for (int j = 2; j <= 3; j++) {
      if (i + j <= word.size ()) {
	int hash = h.HashInput (word.begin () + i, word.begin () + i + j);
	if (h.imap.find (hash) != h.imap.end ()) 
	  fst.AddArc (i, StdArc (hash, hash, StdArc::Weight::One (), i + j));
      }
    }
  }
  fst.SetFinal (word.size (), StdArc::Weight::One ());

  return fst;
}

typedef priority_queue<TokenSet::iterator, 
		       vector<TokenSet::iterator>,
		       TokenIteratorCompare> TokenQueue;

struct Chunk {
 public:
  Chunk (int c, double w, double t) 
    : c_(c), w_(w), t_(t) { }
  int c_;
  double w_;
  double t_;
};


vector<Chunk> RnnDecode (VectorFst<StdArc>& fst, SharedRnnLM& s, int nbest = 1) {
  vector<Chunk> result;
  Heap<TokenSet::iterator, TokenIteratorCompare, false> queue;
  TokenSet pool;
  Token    start (s.hsize, s.max_order);
  pool.insert (start);
  TokenSet::iterator prev = pool.find (start);
  TokenSet::iterator best;
  int key = queue.Insert (prev);
  prev->key = key;
  bool done = false;

  while (!queue.Empty ()) {
    TokenSet::iterator top = queue.Pop ();
    if (fst.Final (top->state) != StdArc::Weight::Zero ()) {
      Token* a = (Token*)&(*top);
      while (a->prev != NULL) {
	result.push_back (Chunk (a->word, a->weight, a->total));
	a = (Token*)a->prev;
      }
      reverse (result.begin (), result.end ());
      return result;
    }

    for (ArcIterator<VectorFst<StdArc> > aiter (fst, top->state);
	 !aiter.Done (); aiter.Next ()) {
      const StdArc& arc = aiter.Value ();
      const vector<int>& map = s.h.imap [arc.ilabel];

      for (int i = 0; i < map.size (); i++) {
	Token nexttoken ((Token*)&(*top), map [i], arc.nextstate);
	nexttoken.weight = s.ComputeNet ((*top), &nexttoken);
        nexttoken.total += top->total - log (nexttoken.weight);
	TokenSet::iterator nextiterator = pool.find (nexttoken);
	if (nextiterator == pool.end ()) {
	  pool.insert (nexttoken);
	  nextiterator = pool.find (nexttoken);
	  key = queue.Insert (nextiterator);
	  nextiterator->key = key;
	} else {
	  //If we have a better result, update all new values.
	  //We hash on the history, state, word, so we do not
	  // need to update these elements.
	  if (nexttoken.total < nextiterator->total) {
	    nextiterator->weight  = nexttoken.weight;
	    nextiterator->total   = nexttoken.total;
	    nextiterator->prev    = nexttoken.prev;
	    queue.Update (nextiterator->key, nextiterator);
	  }
	}
      }
    }
  }

  reverse (result.begin (), result.end ());
  return result;
}


int main (int argc, char* argv []) {
  if (argc != 3) {
    cout << "USAGE: <RNNLM> <TESTSET>" << endl;
    exit (0);
  }
  RnnLMReader m (argv[1]);
  BRnnHash h    = m.CopyVocabHash ();
  SharedRnnLM s = m.CopySharedRnnLM (h);
  s.max_order   = 5;
  string infile = argv [2];
  vector<vector<string> > corpus;
  LoadTestSet (infile, &corpus);

  for (int i = 0; i < corpus.size (); i++) {
    VectorFst<StdArc> fst = WordToFst (corpus [i], h);
    
    vector<Chunk> result = RnnDecode (fst, s);
    for (int j = 0; j < result.size (); j++)
      cout << h.vocab_[result [j].c_].word << " ";
    cout << result [result.size () - 1].t_ << endl;
  }

  return 1;
}