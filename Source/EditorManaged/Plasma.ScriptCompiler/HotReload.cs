using System.Collections.Immutable;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Emit;

namespace Plasma.ScriptCompiler;

// Each chain is tied to the exact PE/PDB originally loaded by a runtime. A runtime can skip
// imports and still apply every intervening delta in order. A rejected chain uses the full PE.
internal sealed record CodeDelta(byte[] Metadata, byte[] IL, byte[] Pdb);
internal sealed record CodeUpdate(Guid BaseMvid, CodeDelta[] Deltas);
// FullReason names why the newest edit could not become an in-place update ("CODE|sentence"), for the editor's Live view.
internal sealed record CodeUpdates(int Version, Guid CurrentMvid, CodeUpdate[] Chains, string? FullReason = null);

internal sealed class HotReload : IDisposable
{
    private readonly List<Track> _tracks = [];
    public CodeUpdates Compile(CSharpCompilation compilation, byte[] pe, byte[] pdb)
    {
        string? fullReason = null;
        foreach (Track track in _tracks.ToArray())
            if (!track.Update(compilation)) { fullReason ??= track.LastFailure; track.Dispose(); _tracks.Remove(track); }
        var latest = new Track(compilation, pe, pdb);
        // Keep the oldest baseline for an ongoing simulation, plus recent baselines for
        // simulations started after an edit. Bound retained compiler graphs and delta bytes.
        if (_tracks.Count >= 8) { _tracks[1].Dispose(); _tracks.RemoveAt(1); }
        _tracks.Add(latest);
        return new(1, latest.Mvid, _tracks.Select(t => new CodeUpdate(t.Mvid, t.Deltas.ToArray())).ToArray(), fullReason);
    }
    public void Dispose() { foreach (Track track in _tracks) track.Dispose(); _tracks.Clear(); }

    private sealed class Track : IDisposable
    {
        private CSharpCompilation _compilation;
        private EmitBaseline _baseline;
        private readonly ModuleMetadata _module;
        private readonly PEReader _pe;
        private readonly MetadataReaderProvider _symbols;
        public Guid Mvid { get; }
        public List<CodeDelta> Deltas { get; } = [];
        public string? LastFailure { get; private set; }
        private bool Reject(string reason) { LastFailure = reason; return false; }
        public Track(CSharpCompilation compilation, byte[] pe, byte[] pdb)
        {
            _compilation = compilation;
            _module = ModuleMetadata.CreateFromImage(pe);
            _pe = new PEReader(new MemoryStream(pe));
            _symbols = MetadataReaderProvider.FromPortablePdbImage(ImmutableArray.Create(pdb));
            var metadata = _pe.GetMetadataReader();
            Mvid = metadata.GetGuid(metadata.GetModuleDefinition().Mvid);
            _baseline = EmitBaseline.CreateInitialBaseline(compilation, _module, DebugInfo,
                method => _pe.GetMethodBody(metadata.GetMethodDefinition(method).RelativeVirtualAddress).LocalSignature, true);
        }
        private EditAndContinueMethodDebugInformation DebugInfo(MethodDefinitionHandle method)
        {
            var reader = _symbols.GetMetadataReader();
            ImmutableArray<byte> slots = [], lambdas = [], states = [];
            foreach (var handle in reader.GetCustomDebugInformation(method))
            {
                var info = reader.GetCustomDebugInformation(handle);
                Guid kind = reader.GetGuid(info.Kind);
                var bytes = ImmutableArray.Create(reader.GetBlobBytes(info.Value));
                if (kind == new Guid("755F52A8-91C5-45BE-B4B8-209571E552BD")) slots = bytes;
                if (kind == new Guid("A643004C-0240-496F-A783-30D64F4979DE")) lambdas = bytes;
                if (kind == new Guid("8B78CD68-2EDE-420B-980B-E15884B8AAA3")) states = bytes;
            }
            return EditAndContinueMethodDebugInformation.Create(slots, lambdas, states);
        }
        public bool Update(CSharpCompilation next)
        {
            if (Deltas.Count >= 64 || Deltas.Sum(d => d.Metadata.Length + d.IL.Length + d.Pdb.Length) > 16 * 1024 * 1024)
                return Reject("C-CHAIN-FULL|The in-place update chain is full (64 updates or 16 MiB).");
            var oldTrees = _compilation.SyntaxTrees.ToDictionary(t => t.FilePath);
            var newTrees = next.SyntaxTrees.ToDictionary(t => t.FilePath);
            if (!oldTrees.Keys.Order().SequenceEqual(newTrees.Keys.Order())) return Reject("C-FILES|Script files were added, removed or renamed.");
            var edits = new List<SemanticEdit>();
            foreach (var (path, oldTree) in oldTrees)
            {
                SyntaxTree newTree = newTrees[path];
                var oldRoot = oldTree.GetRoot();
                var newRoot = newTree.GetRoot();
                if (oldRoot.IsEquivalentTo(newRoot)) continue;
                // Only method bodies may differ. Constructors, fields, properties, attributes,
                // generated descriptors and type structure are a full reload boundary.
                if (!new Shape().Visit(oldRoot)!.IsEquivalentTo(new Shape().Visit(newRoot)!))
                    return Reject("C-FIELDS|Fields, properties, attributes or other type structure changed, so a new type is needed.");
                var oldMethods = oldRoot.DescendantNodes().OfType<MethodDeclarationSyntax>().ToArray();
                var newMethods = newRoot.DescendantNodes().OfType<MethodDeclarationSyntax>().ToArray();
                if (oldMethods.Length != newMethods.Length) return Reject("C-MEMBERS|Methods were added or removed.");
                for (int i = 0; i < oldMethods.Length; ++i)
                {
                    var oldMethod = oldMethods[i]; var newMethod = newMethods[i];
                    if (oldMethod.IsEquivalentTo(newMethod)) continue;
                    // State machines, captured locals and new/deleted local slots need a richer
                    // syntax map. Fall back rather than guess how existing frames should migrate.
                    if (oldMethod.Modifiers.Any(SyntaxKind.AsyncKeyword) || newMethod.Modifiers.Any(SyntaxKind.AsyncKeyword) ||
                        oldMethod.DescendantNodes().Concat(newMethod.DescendantNodes()).Any(n =>
                            n is AnonymousFunctionExpressionSyntax or LocalFunctionStatementSyntax or YieldStatementSyntax))
                        return Reject("C-LAMBDA|Async, lambda, iterator and local-function edits reload fully under the current policy.");
                    Func<SyntaxNode, SyntaxNode?>? syntaxMap = null;
                    if (oldMethod.DescendantNodes().Concat(newMethod.DescendantNodes()).Any(n =>
                        n is VariableDeclaratorSyntax or SingleVariableDesignationSyntax or ForEachStatementSyntax or CatchDeclarationSyntax))
                    {
                        // Stable declaration names and an unchanged syntax shape give an unambiguous
                        // mapping for existing local slots. Literal edits may move source offsets.
                        // Adding/removing locals or control-flow nodes takes the full reload path.
                        var oldNodes = oldMethod.DescendantNodesAndSelf().ToArray();
                        var newNodes = newMethod.DescendantNodesAndSelf().ToArray();
                        if (oldNodes.Length != newNodes.Length) return Reject("C-LOCALS|Local variables or control flow changed shape.");
                        var map = new Dictionary<(int Kind, int Start, int Length), SyntaxNode>();
                        for (int n = 0; n < oldNodes.Length; ++n)
                        {
                            if (oldNodes[n].RawKind != newNodes[n].RawKind) return Reject("C-LOCALS|Local variables or control flow changed shape.");
                            if (oldNodes[n] is VariableDeclaratorSyntax oldLocal &&
                                newNodes[n] is VariableDeclaratorSyntax newLocal && oldLocal.Identifier.ValueText != newLocal.Identifier.ValueText)
                                return Reject("C-LOCALS|A local variable was renamed.");
                            map[(newNodes[n].RawKind, newNodes[n].SpanStart, newNodes[n].Span.Length)] = oldNodes[n];
                        }
                        syntaxMap = node => map.GetValueOrDefault((node.RawKind, node.SpanStart, node.Span.Length));
                    }
                    var oldSymbol = _compilation.GetSemanticModel(oldTree).GetDeclaredSymbol(oldMethod);
                    var newSymbol = next.GetSemanticModel(newTree).GetDeclaredSymbol(newMethod);
                    if (oldSymbol is null || newSymbol is null) return Reject("C-SYMBOLS|A changed method could not be matched to its previous version.");
                    edits.Add(new SemanticEdit(SemanticEditKind.Update, oldSymbol, newSymbol, syntaxMap));
                }
            }
            if (edits.Count != 0)
            {
                using var metadata = new MemoryStream(); using var il = new MemoryStream(); using var pdb = new MemoryStream();
                var result = next.EmitDifference(_baseline, edits, _ => false, metadata, il, pdb);
                if (!result.Success) return Reject("C-EMIT|The compiler could not produce an in-place update for this edit.");
                if (Deltas.Sum(d => (long)d.Metadata.Length + d.IL.Length + d.Pdb.Length) +
                    metadata.Length + il.Length + pdb.Length > 16 * 1024 * 1024) return Reject("C-CHAIN-FULL|The in-place update chain is full (64 updates or 16 MiB).");
                Deltas.Add(new(metadata.ToArray(), il.ToArray(), pdb.ToArray()));
                _baseline = result.Baseline!;
            }
            _compilation = next;
            return true;
        }
        public void Dispose() { _module.Dispose(); _pe.Dispose(); _symbols.Dispose(); }
    }
    private sealed class Shape : CSharpSyntaxRewriter
    {
        public override SyntaxNode? VisitMethodDeclaration(MethodDeclarationSyntax node) =>
            node.WithBody(null).WithExpressionBody(null).WithSemicolonToken(default);
    }
}
