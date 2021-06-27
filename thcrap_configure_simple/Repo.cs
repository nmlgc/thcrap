using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace thcrap_configure_simple
{
    public class RepoPatch
    {
        public Repo Repo { get; set; }
        public string Id { get; set; }
        public string Title { get; set; }
        // Initialized when calling AddToStack
        public string Archive { get; set; }

        public RepoPatch(Repo repo, ThcrapDll.repo_patch_t patch)
        {
            this.Repo = repo;
            this.Id = patch.patch_id;
            this.Title = patch.title;
        }
        private void LoadDependencies(List<Repo> repoList, HashSet<RepoPatch> knownPatches, IntPtr dependenciesPtr)
        {
            if (dependenciesPtr == IntPtr.Zero)
                return;

            var deps = ThcrapHelper.ParseNullTerminatedStructArray<ThcrapDll.patch_desc_t>(dependenciesPtr, 4);
            foreach (var dep in deps)
            {
                string repo_id = dep.repo_id;
                if (repo_id == null)
                    repo_id = this.Repo.Id;

                Repo repo = repoList.Find((Repo it) => it.Id == repo_id);
                if (repo == null)
                    continue;
                RepoPatch patch = repo.Patches.Find((RepoPatch it) => it.Id == dep.patch_id);
                if (patch == null)
                    continue;
                patch.AddToStack(repoList, knownPatches);
            }
        }
        public void AddToStack(List<Repo> repoList, HashSet<RepoPatch> knownPatches)
        {
            if (knownPatches.Contains(this))
                return;
            knownPatches.Add(this);

            ThcrapDll.patch_desc_t patch_desc;
            patch_desc.repo_id = this.Repo.Id;
            patch_desc.patch_id = this.Id;

            IntPtr patch_desc_ptr = ThcrapHelper.AllocStructure(patch_desc);
            ThcrapDll.patch_t patch_info = ThcrapUpdateDll.patch_bootstrap(patch_desc_ptr, this.Repo.repo_ptr);

            this.Archive = Marshal.PtrToStringAnsi(patch_info.archive);
            ThcrapDll.patch_t patch_full = ThcrapDll.patch_init(this.Archive, IntPtr.Zero, 0);

            this.LoadDependencies(repoList, knownPatches, patch_full.dependencies);

            IntPtr patch_full_ptr = ThcrapHelper.AllocStructure(patch_full);
            ThcrapDll.stack_add_patch(patch_full_ptr);

            ThcrapHelper.FreeStructure<ThcrapDll.patch_desc_t>(patch_desc_ptr);
            ThcrapHelper.FreeStructure<ThcrapDll.patch_t>(patch_full_ptr);
        }
    }

    public class Repo
    {
        public IntPtr repo_ptr;
        public string Id { get; set; }
        public string Title { get; set; }
        public List<RepoPatch> Patches { get; set; }
        public static List<Repo> Discovery(string start_url)
        {
            IntPtr repo_list = ThcrapUpdateDll.RepoDiscover(start_url);
            if (repo_list == IntPtr.Zero)
                return new List<Repo>();

            var out_list = new List<Repo>();
            int current_repo = 0;
            IntPtr repo_ptr = Marshal.ReadIntPtr(repo_list, current_repo * IntPtr.Size);
            while (repo_ptr != IntPtr.Zero)
            {
                out_list.Add(new Repo(repo_ptr));
                current_repo++;
                repo_ptr = Marshal.ReadIntPtr(repo_list, current_repo * IntPtr.Size);
            }
            
            ThcrapDll.thcrap_free(repo_list);
            return out_list;
        }

        private Repo(IntPtr repo_ptr)
        {
            ThcrapDll.repo_t repo = Marshal.PtrToStructure<ThcrapDll.repo_t>(repo_ptr);

            Id = repo.id;
            Title = repo.title;
            this.repo_ptr = repo_ptr;

            var patchesList = ThcrapHelper.ParseNullTerminatedStructArray<ThcrapDll.repo_patch_t>(repo.patches);
            Patches = new List<RepoPatch>();
            foreach (var it in patchesList)
                Patches.Add(new RepoPatch(this, it));
        }
        ~Repo()
        {
            ThcrapDll.RepoFree(repo_ptr);
        }
    }
}