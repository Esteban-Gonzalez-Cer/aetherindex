// Prints the tokens Anserini indexes for each input line (DefaultEnglishAnalyzer: Lucene
// StandardTokenizer, English possessive filter, lowercase, Lucene English stopwords,
// Porter stemmer), one output line per input line, tokens separated by spaces.
//   java -cp anserini-fatjar.jar:CLASSES LuceneTokens INPUT OUTPUT [porter|none]
import io.anserini.analysis.DefaultEnglishAnalyzer;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import org.apache.lucene.analysis.Analyzer;
import org.apache.lucene.analysis.TokenStream;
import org.apache.lucene.analysis.tokenattributes.CharTermAttribute;

public class LuceneTokens {
  public static void main(String[] args) throws Exception {
    String stemmer = args.length > 2 ? args[2] : "porter";
    Analyzer analyzer = DefaultEnglishAnalyzer.newStemmingInstance(stemmer);
    try (BufferedReader in = Files.newBufferedReader(Paths.get(args[0]), StandardCharsets.UTF_8);
         BufferedWriter out = Files.newBufferedWriter(Paths.get(args[1]), StandardCharsets.UTF_8)) {
      for (String line; (line = in.readLine()) != null; ) {
        StringBuilder tokens = new StringBuilder();
        try (TokenStream stream = analyzer.tokenStream("contents", line)) {
          CharTermAttribute term = stream.addAttribute(CharTermAttribute.class);
          stream.reset();
          while (stream.incrementToken()) {
            if (tokens.length() > 0) tokens.append(' ');
            tokens.append(term);
          }
          stream.end();
        }
        out.write(tokens.toString());
        out.newLine();
      }
    }
  }
}
