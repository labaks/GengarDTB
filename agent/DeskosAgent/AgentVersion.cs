using System.Reflection;

namespace DeskosAgent;

internal static class AgentVersion
{
    public static readonly string String =
        Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "0.0.0";
}
