import { useState, type ReactNode } from "react"
import { Upload } from "@marquee/ui/lib/icons"
import { cn } from "@marquee/ui/lib/utils"

/** Drag files onto the area to add them to the content folder. */
export function DropZone({
  onFiles,
  label,
  children,
  className,
}: {
  onFiles: (files: File[]) => void
  label: string
  children: ReactNode
  className?: string
}) {
  const [over, setOver] = useState(0)
  return (
    <div
      className={cn("relative", className)}
      onDragEnter={(e) => {
        if (!e.dataTransfer.types.includes("Files")) return
        e.preventDefault()
        setOver((n) => n + 1)
      }}
      onDragLeave={() => setOver((n) => Math.max(0, n - 1))}
      onDragOver={(e) => {
        if (e.dataTransfer.types.includes("Files")) e.preventDefault()
      }}
      onDrop={(e) => {
        e.preventDefault()
        setOver(0)
        const files = Array.from(e.dataTransfer.files)
        if (files.length) onFiles(files)
      }}
    >
      {children}
      {over > 0 && (
        <div className="pointer-events-none absolute -inset-2 z-20 grid place-items-center rounded-2xl border-2 border-dashed border-ring bg-background/80 backdrop-blur-sm">
          <div className="flex flex-col items-center gap-2 text-sm">
            <Upload className="size-6" />
            {label}
          </div>
        </div>
      )}
    </div>
  )
}
