# 指定回数単体テストを繰り返す。エラーとなったら停止。
# 無指定時は100回
# 開発ツールチェインに実行パスが設定されていること

$cnt = 100
try {
    if($args[0]){
        $cnt = [int]$args[0]
    }
}
catch {}

1..$cnt | Foreach-object {
    vstest.console.exe .\x64\Release\TestSimplePipe.dll /TestCaseFilter:Priority!=2 /Parallel
    echo $_
    if(!$?){
        break
    }
}